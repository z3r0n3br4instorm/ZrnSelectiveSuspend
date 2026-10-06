# Track C: results on the reference laptop

MacBookPro9,1, Linux 7.2.2, NVIDIA 470.256.02, X11 session running on the
Intel GPU with the NVIDIA GPU as an inactive provider. Run on 2026-10-06.
Raw log: `docs/track-c/suspend-cycle.log`. Script: `tests/hw_suspend_cycle.py`.

## What was found before switching anything

| Check | Result |
| :--- | :--- |
| gmux | Classic, version 1.9.35, I/O ports `0x700`–`0x7fe` |
| `zssd` backend detection | `apple-gmux`, removal not supported |
| gmux power port `0x750` | Reads `0x03` while the dGPU is powered |
| gmux external-port mux `0x740` | Reads `0x03`: the external port is routed to the dGPU |
| Root port `00:01.0` | **No AER capability, no hot-plug.** The AER-masking idea in `SPEC.md` has nothing to act on here |
| Processes holding the dGPU | `Xorg`, and the `nvidia-persistenced` service |
| `zssctl detach` with the real backend | Refused with `ZSSDetachBlocked`, naming both; nothing switched |
| Detach by an unauthorised user | Refused with "permission denied" |

## The suspend cycle

Sequence: stop `nvidia-persistenced`; switch away from the X session's VT;
unbind the audio function; save 256 bytes of PCI configuration; write
`suspend` to `/proc/driver/nvidia/suspend`; gmux power off; wait five
seconds; gmux power on; restore configuration; write `resume`; rebind audio;
switch back; restart the service.

Run four times, all clean (three with a 5-second cut, one with 90 seconds):

| Step | Observed |
| :--- | :--- |
| Driver suspend | 0.04–0.05 s |
| Power off | Card gone from the bus within 0.1 s; vendor ID reads `0xffff`; port `0x750` reads `0x00` |
| Root port link while off | Still reports a link (`2.5GT/s` or `5GT/s`, x8); it is not a usable sign of power |
| Power on | Card answers within 0.1 s, blank: command register `0x0000`, BAR0 `0` |
| Configuration restore | All but two bytes match. `0x86`/`0x8a` and `0xaa` are in the read-only link capability and status registers |
| Driver resume | 0.14 s |
| Kernel log | No NVRM or Xid messages; only the audio function re-registering |
| X session | Survived every cycle |
| Total | About 8.5 s, of which 5 s is the deliberate wait and 1 s the VT switch settling |

After the cycles the dGPU rendered the test scene byte-identically with and
without the layer, and within one level of the software renderer.

## Migration across a real power cut

With `zssd` on the dry-run backend: `zss-testapp` running on the dGPU under
the layer was migrated to the Intel GPU, the dGPU was power-cycled as above
while the application kept rendering, and the application was migrated back.

| Observed | Result |
| :--- | :--- |
| NVIDIA device files held by the application after migrating away | 0 |
| Frames rendered on Intel during the cycle | 155 |
| Application | Stayed alive, exit status 0 |
| NVIDIA device files held after returning | 18 |
| All 300 frames against an undisturbed run on the dGPU | Within tolerance 2, largest difference 1 |

## Power saved

One run on battery, the whole of it on the text console so the display and
desktop load were the same throughout: 45 s with the dGPU powered and idle
(performance state P8), 90 s powered off, 45 s powered again. Battery
current and voltage were sampled twice a second; the first 12 s of each
phase are dropped because the battery gauge averages over several seconds.

| Phase | Samples | Median | Mean | Range |
| :--- | :--- | :--- | :--- | :--- |
| dGPU on, before | 63 | 19.8 W | 19.6 W | 15.7–24.5 W |
| dGPU off | 151 | 18.1 W | 18.0 W | 14.8–26.5 W |
| dGPU on, after | 63 | 22.7 W | 23.7 W | 16.4–36.4 W |

**Powering the dGPU off saved roughly 2 W**: 1.7 W against the phase before,
4.6 W against the phase after, 2.4 W against both together. The "after"
phase is the less trustworthy one; it includes whatever the driver and the
system do right after a resume. This is a single run with a noisy gauge, so
the figure is an estimate, not a measurement to quote to a decimal place.

For scale: at about 20 W total, 2 W is around a tenth of the idle draw. The
dGPU was already idling in its lowest performance state, so this is the
floor of what the cut is worth; the saving against a dGPU that something is
keeping busy would be larger.

The 90-second cut was the longest so far and came back as cleanly as the
5-second ones.

## The dGPU off while the X session is on screen

Script: `tests/hw_off_in_x.py`. Log: `docs/track-c/off-in-x.log`.

The driver was suspended and power cut on the text console as before, and
then the console was switched back to the X session with the dGPU still off.

| Observed | Result |
| :--- | :--- |
| Switching back to the X session's VT | Returned at once |
| First request to X afterwards (`xset q`) | **No answer in 15 s** |
| Xorg process | Still running, not crashed |
| Kernel log, X log | No error; X simply stopped |
| dGPU powered on and driver resumed | X answered again within 2 s, display queries included |

**X freezes for as long as the dGPU is off.** With the NVIDIA driver loaded,
X tries to use the card when it regains the screen and waits for it. It does
not crash and loses nothing: it carries on as soon as the card is back.

So on this laptop, with the NVIDIA driver in X (needed for the external
display port), the dGPU can be power-cycled but cannot stay off while the
desktop is in use. Keeping it off would need X not to load the NVIDIA
driver, or a display server that tolerates the card going away.

## Wake on touch, with the patched driver

Patch: `patches/nvidia-470.256.02-wake-on-touch.patch`. Script:
`tests/hw_wake_on_touch.py`. Log: `docs/track-c/wake-on-touch.log`.

**Why X froze.** The open part of the 470 driver takes a lock when it is
suspended through `/proc/driver/nvidia/suspend` and holds it until resume
(`nv_system_pm_lock` in `nvidia.ko`, and a twin, `nvkms_pm_lock`, in
`nvidia-modeset.ko`). Every call into the driver waits for that lock in a
loop with no timeout. The driver was written for whole-system sleep, where
callers are frozen; it has no way to ask for a wake-up. X was never reaching
the missing hardware: it was spinning at the lock.

**The patch** (55 added lines, four files) replaces the spin in both modules:
a caller that finds the lock held increments
`/sys/module/nvidia/parameters/zss_wake_requests` and sleeps until the lock
is released. Nothing changes while the driver is running. A root helper
watches the counter and, when it moves, powers the card on and resumes the
driver.

The patched modules were built outside the system's DKMS tree, which is
untouched, and installed over the five module files in `updates/dkms`, with
the stock files kept in `/var/lib/zss/nvidia-stock-<kernel>/`. A kernel
update will rebuild and reinstall the stock driver.

| Test | Result |
| :--- | :--- |
| Boot with the patched modules | Normal; X started, both providers present |
| The sequence that froze X before: suspend and cut power on the console, return to the desktop with the card off | X called into the driver 0.4 s after getting the screen back (`nvidia_ioctl`); wake request seen; card on and driver resumed 0.54 s later; X fine, screen on the desktop |
| Suspend and cut power **without leaving the desktop** | Driver suspended in 0.05 s with X on screen; no console switch needed |
| X idle with the card off | No call into the driver for 12 s, until a query was made on purpose |
| `xset q` with the card off | Answered at once; did not touch the driver |
| `xrandr --listproviders` with the card off | Called into the driver (`nvkms_ioctl`); wake request; card back in 0.50 s; the query completed in 2.0 s |
| X and kernel afterwards | Display queries fine; kernel log empty |

**What it means.** The dGPU can be off while the desktop is on screen and in
use. Ordinary X activity does not wake it; asking X about displays does, and
then the caller waits about two seconds instead of freezing. The console
switch is not needed at all.

Seen but not yet understood: the counter went from 2 to 343 during the
second of the resume. Something retries in a tight loop while the driver is
coming back, most likely the page-fault path, which returns "retry" rather
than sleeping. Harmless in this run, but it should sleep too.

Not yet known: how long the card stays off in real use (the 12 s was cut
short by the test's own query), whether `nvidia-persistenced` wakes it when
left running, and whether plugging in an external display wakes it.

## Known limitation: a monitor plugged in while the dGPU is off

A card with no power cannot report a hot-plug, and nothing asks X about
displays by itself, so a monitor connected while the dGPU is off is not
noticed. Any display query (opening the display settings, `xrandr`) or
`zssctl on` wakes the card, and the monitor then appears. The gmux has a
hot-plug interrupt that might be used to wake the card automatically; whether
it can be read without disturbing the kernel's gmux driver has not been
investigated. Not yet tried on the hardware.

## What this settles

- **Suspending the driver and resuming it brings the card back from a real
  cold power cut.** The driver re-initialises the card itself; no VBIOS
  handling of ours is needed.
- **Reading the vendor ID is a correct power check**, and port `0x750` could
  serve as a second one.
- **X does not have to be stopped**, only switched away from, and it keeps
  its NVIDIA driver loaded throughout.
- The order used to restore configuration (everything from `0x10` up, then
  `0x0c`–`0x0f`, then the command register) is now the order in
  `src/daemon/backend.c`.

## What it changes in the design

The daemon treats any display server on the GPU, and any process outside the
layer, as a blocker. For a suspend-in-place device that is stricter than it
needs to be: Xorg and `nvidia-persistenced` held the device the whole time
and the cycle was clean, because the VT switch and stopping the service are
enough. To run this sequence through `zssd` rather than by hand, the
suspend-in-place path needs to do those two things itself and stop treating
idle holders as blockers. That is a change to the specification and has not
been made.

## Not done

- **Power saving is measured once only.** Repeating it several times, and with the dGPU busy beforehand, would tighten the figure.
- **The unbind strategy** (PCI remove, power cycle, rescan) was not tried.
- **A power cut under the active driver** (the real device-loss case) was not
  approved and not run.
- **Longer off periods**, and a cycle while an application is using the dGPU
  outside the layer.
