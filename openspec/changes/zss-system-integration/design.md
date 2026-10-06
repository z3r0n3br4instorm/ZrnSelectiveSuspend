## Context

See `proposal.md` for motivation. What exists and was measured on the reference laptop (`docs/track-c.md`):

- `zssd`, `zssctl`, the graphics layer, and an `apple-gmux` power backend whose suspend, power-cut, power-on and resume sequence has run cleanly many times by hand.
- A 55-line patch to the open part of NVIDIA 470.256.02 (`patches/`). With it, a caller that finds the driver suspended increments `/sys/module/nvidia/parameters/zss_wake_requests` and sleeps instead of spinning forever.
- With that patch: the driver suspends in 0.05 s with X on screen and no console switch; X idle did not call into the driver for the 12 s observed; a display query did, and completed 2.0 s later after a helper powered the card on (0.5 s).
- The patch was installed by overwriting five module files. DKMS knows nothing about it, so the next kernel update restores the stock driver.
- During the half-second resume the counter rose by 341: one path retries in a tight loop instead of sleeping.
- This machine already carries a package-manager hook that re-patches another DKMS driver after updates (`72-broadcom-wl-repatch.hook`), so the pattern is established here.

Constraints: the user needs external displays, so the NVIDIA driver stays loaded in X. Bootloader configuration must not be touched. The session doing this work has no way to load or test kernel code except by rebooting the user's machine.

## Goals / Non-Goals

**Goals:**

- `zssctl off` and `zssctl on` do on the reference laptop what the test script does, and the card comes back by itself when needed.
- A kernel update or a driver package update cannot bring the freeze back silently.
- A failed driver build or load never leaves the machine without a working discrete GPU after reboot.
- Everything the installer adds, the uninstaller removes.

**Non-Goals:**

- New power backends. Machines without a classic gmux get migration only.
- Validating the patch against other NVIDIA versions. The mechanism for a validated list is built; the list starts with one entry.
- Detecting a monitor plugged in while the card is off (see Risks).
- Any change to the graphics layer.

## Decisions

### D1. No separate kernel module

Everything needed in the kernel is the patch to NVIDIA's own open kernel-interface code. A separate ZSS module would have nothing to do that the patch and the daemon do not already do: gmux power is two port writes, PCI configuration is saved and restored through sysfs, and both ran reliably from user space.

*Alternative considered:* a GPL module that hooks the driver's entry points from outside, leaving NVIDIA's source untouched. Rejected: it would probe private functions of a proprietary module, break on any rename, and the driver's own source is the honest place for a change in the driver's behaviour.

A module remains the right tool if a later change needs to act inside the kernel's power-management path, for instance to make the PCI core restore configuration itself. Nothing measured so far needs it.

### D2. The patch is delivered through DKMS

DKMS applies patches listed in a package's `dkms.conf` on every build. The installer copies the patch into the driver's source tree and adds the `PATCH` lines, so a rebuild for a new kernel includes it with no further help.

Those are files owned by the driver package, so a package update overwrites them. A package-manager hook re-applies them and triggers a rebuild after the driver package is installed or upgraded. On Arch this is a pacman hook; the tool behind it (`zss-nvidia-patch apply|remove|status`) is distribution-neutral so other hook systems can call it.

The tool applies the patch only if the driver version is in `validated-versions`, and first checks that the patch applies cleanly; if either fails it leaves the driver alone.

*Alternative considered:* overwrite the built modules, as was done by hand. Rejected: lost at the next kernel update, and invisible to DKMS.

### D3. Rollback at boot

`zss-nvidia-patch apply` keeps the stock modules for the running kernel before the first patched build is installed. A one-shot service ordered before the display manager loads the driver; if that fails it restores the stock modules, loads them, and logs why. The hook keeps a stock copy per kernel, since after a kernel update the previous copy no longer fits.

### D4. Patch revision 2

- **Sleep everywhere.** Find the path that retried 341 times during a resume (the page-fault path returns "retry" at once) and make it wait for the resume instead.
- **A wake request that can be waited for.** Add a file under `/proc/driver/nvidia/` that supports `poll`, so the daemon blocks until a request arrives. The symbols this needs are available to a non-GPL module; the ones for sysfs notification and uevents are not, which is why revision 1 used a plain counter. The counter stays, for diagnostics and as the fallback.
- **Wake-capability marker.** The presence of that file is how the daemon and `zssctl status` tell that the running driver has wake support.

### D4a. Patch revision 3: who is waiting

Revision 2 says that someone is waiting, not who. On the laptop the first requested power-off lasted 33.7 s before an unnamed caller ended it, and the user's expectation is plain: a device switched off by command stays off. So the wake file now also lists the process id of every waiting caller (a fixed table of 32 under a spinlock, filled on entry to the wait and cleared on leaving it), and the daemon decides:

| Powered off by | Caller | Result |
| :--- | :--- | :--- |
| idle timer | anyone | power on |
| request | display server | power on: the desktop is stopped while it waits |
| request | anything else | stays off; the caller sleeps in the driver until `zssctl on`, and is logged |
| request | unknown (older patch, or more waiters than the table lists) | power on, since the display server may be among them |

A waiting caller is in the same position as a frozen one (D6a), except that it stopped itself by touching the device.

### D4b. Hiding a device that is off

Measured on the laptop with revision 3: a GPU switched off on request came back within 7 to 30 s, every time because the display server called the driver. A controlled run showed why. A plain window does not wake it, nor does a Vulkan program limited to the Intel driver; loading NVIDIA's Vulkan driver does, with no window at all: the client library asks the X server, and X makes the call (`nvidia_ioctl`). EGL programs reach the driver themselves through the DRM node and hang, and `nvidia-smi` hangs reading `/proc/driver/nvidia/params`. The display server has to be let through (D4a), so the rule alone cannot keep the device off.

So a device switched off on request is hidden, as an unplugged one would be. The daemon bind-mounts an empty file over:

- the device's nodes in `/dev` and `/dev/dri`, and for the NVIDIA driver its control nodes;
- for a vendor driver, the loader files that name it (Vulkan ICD manifests, EGL vendor files); Mesa's are shared between devices and stay;
- for the NVIDIA driver, its files under `/proc/driver/nvidia`, except the suspend and wake files.

The mounts carry the device address in their source, so `gpu_unhide` finds them in `/proc/self/mountinfo` whoever made them; they are undone at power-on, on a clean stop, and by `zssd --recover`, and cannot outlive a reboot. `hide_while_off` in the configuration is `auto`, `no`, or extra paths.

With this, on the laptop: `vulkaninfo`, `vkcube`, `glxinfo`, `eglinfo` and `zss-run` programs ran on Intel, `nvidia-smi` failed at once with an NVML error, and the device stayed off until `zssctl on`. A display query through X still wakes it, which is wanted.

Hiding does not stop the display server calling for reasons of its own. Closing an application that the layer had moved off the device made X call the driver about 100 ms later, every time, although nothing of NVIDIA's was left in the process; giving the driver a connection of the layer's own (kept, since it lets the layer sever the link when it releases a driver) did not change that, so the state belongs to the window inside X. Such calls cannot be prevented from outside X, so they are served: the daemon powers the device, lets the call finish, and switches it off again after 1.5 s, without unhiding it or restarting anything. The state stays `powered-off`; `served=` counts the occasions. Calls less than 20 s apart double the stay, up to a minute, so that a burst costs one power cycle. If the device has begun driving a display by then, it is given back completely instead. On the laptop: on after 0.22 s, off again 1.5 s later, and a plain `vkcube` started afterwards ran on Intel.

*Alternative considered:* have the driver fail calls instead of waiting while held off. Rejected for now: the X server would receive those failures too, and how its driver takes them cannot be tried without risking the session.

### D5. Off and on in the daemon

```
 zssctl off / idle timer
   │ refuse if: driving a display, no wake support + display server
   ▼
 migrate or park applications ─► stop configured services, freeze what cannot be moved
   ─► unbind audio function
   ─► save PCI configuration (also to /run/zss/) ─► suspend driver ─► power off
   ─► state powered-off, start watching for wake requests

 wake request / zssctl on / daemon stopping
   ▼
 power on ─► restore configuration ─► resume driver ─► rebind audio ─► start services, thaw
   ─► state attached
```

Applications that were migrated away stay where they are after a wake; `zssctl attach` (or `on --return`) brings them back. Waking is frequent and cheap, migration is not, and a display query is no reason to move an application.

The state while off is the existing `powered-off`. No new state is needed: from the daemon's side this is a suspend-in-place detach whose attach can be triggered by the driver.

### D5a. Progress, and the text console

Every power transition is narrated one step at a time, each line prefixed `[ZrnSelectiveSuspend]`. A line goes to three places at once: the daemon's log, the `zssctl` that asked (a `progress` message, printed as it arrives), and the text console when the screen is on it.

With a wake-capable driver the screen never leaves the desktop, so there is no console to print on; the terminal running `zssctl` is where the user sees it. `zssctl off --console` asks for the console on purpose: the daemon switches to a virtual terminal of its own, prints there, and stays there while the device is off. A key pressed on that console, `zssctl on`, or a wake request powers the device on and returns the screen. This is also the only way to power off under a display server whose driver has no wake support: the desktop is not shown, so it is allowed to wait.

Returning to the desktop makes X call into its driver at once, which wakes a wake-capable device. That is why console mode keeps the screen on the console for as long as the device is off, rather than switching back.

### D6. Who may hold the device

| Holder | Power-off allowed? |
| :--- | :--- |
| Application under the layer, migratable | Yes, after it has been moved |
| Application under the layer, not migratable | On request: yes, frozen while the device is off. Idle timer: no |
| Process outside the layer that is not a display server or a listed service | On request: yes, frozen while the device is off. Idle timer: no |
| The terminal (or any ancestor) of the `zssctl` that asked | No, unless `--console` |
| Service listed in `stop_services` (default `nvidia-persistenced` for the NVIDIA driver) | Yes; stopped and restarted around the cut |
| Display server, driver has wake support, no connected display on this GPU | Yes |
| Display server otherwise | No |

"Driving a display" is read from the kernel's connector status for the GPU's DRM device.

### D6a. Freezing what cannot be moved

A process outside the layer cannot be migrated, but it does not have to block: frozen in a cgroup of its own (the mechanism already used for parked applications) it makes no call into the driver, and the driver saves and restores what the process had on the device across the power cut, exactly as across a system sleep. It is thawed after the driver is resumed, whether by `zssctl on`, a wake request, a clean stop or `zssd --recover`; the frozen process ids are written into the recovery marker for the last case.

Limits, on purpose:

- **Only on request.** The idle timer treats such a process as use of the device. Freezing someone's program because a timer ran out is not something a user asked for.
- **Not the requester's own terminal.** If the terminal `zssctl off` was typed into uses the device, freezing it would leave nobody to type `zssctl on`. That is refused with the reason; `--console` lifts it, because a key press on the console powers the device on.
- **Not for a detach.** A detached device may be removed and never return, and a frozen process would then wait forever.

A frozen process shows a window that does not repaint. Nothing wakes the device on its behalf: it is frozen, so it never reaches the driver. A window manager or compositor that uses the device would freeze the whole desktop; the way out is another virtual terminal and `zssctl on`.

### D7. Idle timer and damping

`idle_timeout` in the configuration file, off by default. The daemon counts time with no client and no connected display. After a wake it waits `idle_timeout` again; if three consecutive power-offs are each ended by a wake within a minute, the wait doubles, up to an hour, and resets after a power-off that lasts. `zssctl status` shows the wake count and the current wait. Without this, anything that polls display state would cycle the card every few seconds.

### D8. Not leaving callers stranded

A caller sleeping on the suspended driver waits for a resume that only ZSS will issue. So:

- on a clean stop, the daemon powers every device on before exiting;
- the saved PCI configuration is written to `/run/zss/` when power is cut, and the service's `ExecStopPost` runs `zssd --recover`, which powers on and resumes any device found off, covering a crash or a kill;
- the unit is `Restart=on-failure`.

### D9. Service, configuration, access

`zssd.service` runs as root. `/etc/zss/zssd.conf` holds: managed GPUs and their backends (or `auto`), `idle_timeout`, `default_target`, `allow_software`, `stop_services`, `group`. Command-line options keep working and override the file. The `zss` group is created at install; the socket stays at `/run/zss/zssd.sock`.

### D10. Installer

`packaging/install.sh`, plus an Arch `PKGBUILD` that does the same through the package manager.

1. **Report** (also available alone as `--check`): power backend found, driver and version, whether that version is validated, whether the driver is in the initial ramdisk.
2. **Install** binaries, launcher, layer manifest, unit, default configuration, group.
3. **Driver patch**, only if a backend and a validated driver were found, and only after an explicit yes: apply through DKMS, keep stock modules, install the hook and the boot check. Says that a reboot is needed.
4. Never edits bootloader configuration. Regenerates the initial ramdisk only when the driver is in it, and says so first.

`packaging/uninstall.sh` reverses each step in the opposite order.

### D11. Testing

| What | Where |
| :--- | :--- |
| Daemon off/on sequence, holder rules, idle timer, damping, recovery | Host, with a fake backend and a fake wake file whose path the daemon takes from its configuration |
| Patch tool: version check, clean-apply check, apply, remove, status | Host, against a copy of the driver source in a temporary directory |
| Installer `--check`, install into a staging root, uninstall leaves nothing | Host |
| Service start, group permissions, `ExecStopPost` recovery | QEMU guest |
| Patch revision 2 builds for the running kernel | Host; build only |
| Patched driver loads; `zssctl off` / `on`; wake by display query; idle run; daemon killed while off | Reference laptop, each needing a reboot or the user's go-ahead |

## Risks / Trade-offs

- **A monitor plugged in while the card is off is not noticed.** The card cannot report a hot-plug when it has no power, and nothing queries X by itself. Any display query, or `zssctl on`, wakes the card and the monitor then appears. The gmux has a hot-plug interrupt that might be usable; whether it can be read without disturbing the kernel's gmux driver is an open question. Until then this is a documented limitation.
- **The patch is tied to one driver version** → Applied only to validated versions; anything else is left stock and reported.
- **A package update between the hook running and the next boot leaves stock modules installed** → The hook rebuilds immediately; `zssctl status` and the daemon log report missing wake support, and power-off under X is refused rather than attempted.
- **A patched driver that builds but misbehaves only when suspended** cannot be caught by the boot check → The patch leaves the running path untouched, the daemon restores power on exit, and the uninstaller restores the stock driver.
- **Something on the desktop may query displays constantly**, keeping the card on → Damping keeps it from cycling; the wake count makes the cause visible. How often this happens in real use is not yet measured.
- **A frozen process has not been through a real power cut yet.** In the tests the device is a file. That the NVIDIA driver restores a frozen client's state after the card lost power is expected from its system-sleep behaviour and is to be confirmed on the laptop.
- **Suspending under a live X without a console switch has run a handful of times only** → Hardware tests repeat it, including a long idle run and a wake under load.
- **Editing a package-owned `dkms.conf`** is unusual → It is the mechanism DKMS provides, the edit is two lines between markers, and removal restores the file byte for byte.
- **Root-only port and configuration access from user space can race the kernel's gmux driver** → Unchanged from `zss-happy-path`; no collision seen in any run.

## Migration Plan

On the reference laptop the hand-installed modules are replaced by a DKMS build of revision 2 through the installer; the stock copy made earlier in `/var/lib/zss/` is reused. One reboot. Rollback is `packaging/uninstall.sh`, or from a console the single restore command already documented.

## Open Questions

- Can the gmux hot-plug interrupt be observed safely to wake the card when a monitor is plugged in? Deferrable: the limitation is documented and there is a manual way out.
- What default should `stop_services` have for drivers other than NVIDIA's? Deferrable: only the NVIDIA path can power off today.
