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

## The kernel module on the laptop (7 October 2026)

`zss.ko` 0.1.0 was installed through the installer (DKMS) and loaded by the
service. The daemon picked the `zss-kmod` backend by itself; the module chose
`gmux` and `quiesce=external`, and took both functions of the slot (the GPU on
`nvidia`, the HDMI audio on `snd_hda_intel`). Loading changed nothing: state
`on`, both functions `online`, `answers` 1.

Two off/on cycles through `zssctl`, under the running X session:

| Step | Result |
| :--- | :--- |
| Driver suspended (user space, as before) | 0.13 s |
| Power cut by the module | 0.03 s; bus reads `ffff`; both functions `offline` |
| Whole `zssctl off` | 0.29 s |
| Ten seconds off | still off |
| Power restored by the module | 0.06 s |
| Driver resumed | 0.11 s; `nvidia-smi` answers, audio function bound again |
| Kernel log | the module's four lines; no warning from it |

The 256 bytes of configuration space of each function were compared before
and after the second cycle. The standard header is identical. Three bytes in
the PCI Express capability differ (0x85, 0x86 and 0x8a on the GPU; 0x85 and
0x8a on the audio function). They sit in the link capability and link status
registers, which the hardware reports after training the link and software
does not write; that reading has not been checked against the register
definitions bit by bit.

This laptop has no IOMMU enabled (`iommu=no`), so nothing confines the GPU's
memory access there, with or without ZSS.

Not run on the laptop: unloading the module while the GPU is off, and the
loss guard firing for real.

## Power cut under the running driver (7 October 2026, 11:53)

The test that had never been run. `zss-run vkcube` was rendering on the NVIDIA
card under the X session; `tests/hw_surprise_cut.py cut` then switched the
gmux power rail off directly. Nothing was suspended and nothing was told. Full
log: `docs/track-c/surprise-cut.log`.

| After the cut | What happened |
| :--- | :--- |
| 151 ms | The kernel module reported the device lost and marked both functions offline |
| under 0.9 s | The daemon's state was `lost`; X was listed as holding a stale handle |
| 3.5 s | `vkcube`'s state had been rebuilt on the Intel GPU. **Its window was frozen all the same** (see below) |
| about 5 to 9 s | One X query went unanswered for more than 2 s; every later one was answered |
| same time | The NVIDIA driver logged `Xid 79 ... GPU has fallen off the bus` against the X server |
| 27 s | Machine up, X answering, `vkcube` process alive, no kernel warning beyond NVIDIA's own lines |

The layer's own account: `abandoning the lost device on NVIDIA GeForce GT
650M: a thread is still inside its driver`, `2 submission(s) in flight were
issued again`, `recovered after submit 1266 onto Intel(R) HD Graphics 4000;
0 object(s) lost their contents; rebuild took 150 ms`. Most of the 3.5 s is
the three-second grace the layer gives a thread stuck in the dead driver
before it abandons that driver and carries on.

What this shows: on this laptop, with this driver, the machine and the display
server survive a card that loses power without warning, with nothing standing
between the driver and its registers. The loss was noticed, the device marked
and the application's state rebuilt elsewhere.

**The application did not survive in any useful sense.** The first write-up of
this test said `vkcube` was running on Intel. It was not: the user saw a
frozen window, and the process used no CPU. A backtrace showed its only
rendering thread still inside NVIDIA's library, in `vkQueuePresentKHR`,
waiting in `xcb_wait_for_special_event` with no timeout for the X server to
acknowledge a present. X's NVIDIA side had died with the card, so that event
would never come. The layer had waited three seconds, abandoned the dead
driver and rebuilt everything on Intel, for a thread that never returned to
use it.

Shutting that X connection from a debugger did bring the thread out of the
wait at once. The process then exited; why was not established (the dead
driver's answer to the present reached the application unfiltered, which is
one possible cause).

The fix that followed, in the layer:

- Each driver already had a connection to the X server of the layer's own.
  When a device is reported gone, the layer now shuts the connections of that
  device's driver before waiting for threads to leave it, so a thread waiting
  there fails instead of waiting for ever. Xlib surfaces are handed to the
  driver as xcb ones for the same reason.
- Anything a driver answers about a device already known lost, and anything
  at all from a thread that was left behind in an abandoned driver, is treated
  as the loss it is: the call is repeated on the rebuilt device.

That fix has passed the QEMU suites only. It has **not** been run against the
failure it is for: that needs a reboot (the NVIDIA driver is dead until then)
and another power cut.

What it does not show:

- **The card coming back.** The rail was left off. The NVIDIA driver has
  declared the GPU lost, and it cannot be rebound while X holds it, so the
  expected way back is a reboot. Powering the rail on again was not tried.
- **A card under heavier use**, more than one application, or a display
  driven from the card at the moment of the cut.
- **`vkcube` still holds its old `/dev/nvidia*` handles**: the abandoned
  driver stays loaded in the process, as designed for this case.
- Whether the picture on screen kept moving was not checked by the script.

## Trying to bring the card back without a reboot (7 October 2026, 12:05)

Log: `docs/track-c/revive.log`. The daemon was stopped first so that it would
not act on the card's return.

| Step | Result |
| :--- | :--- |
| Power rail switched on through the gmux | The card answered at once (`de10`), with a blank configuration |
| Kernel module asked to take the device back | Accepted: PCI configuration restored (command register and base addresses as before), both functions online, `needs_rebind` set |
| `nvidia-smi`, nothing else changed | `Unable to determine the device handle ... Unknown Error`: the driver still treats the GPU as lost |
| `suspend` written to `/proc/driver/nvidia/suspend`, hoping its resume would re-initialise the card | **Never returned.** The writer spins in the kernel at full CPU; `resume` waits behind it. `nvidia-modeset` logged `Failed to query display engine channel state` |

So the card can be revived and the NVIDIA 470 driver cannot, in place. Once it
has logged "GPU has fallen off the bus" it does not look at the device again,
and its sleep path does not cope with that state. The attempt left the machine
worse off than before it: one CPU core busy in the kernel, and the driver
locked in a suspend that will not finish, so the X server will stop the moment
it next calls into the driver. Only a reboot clears that.

What remains to try, on a fresh boot: after a loss, leave the driver alone
until nothing holds the device, then unbind it and bind it again. For the
NVIDIA driver under X that means the X server has to be restarted (a log-out),
which is still less than a reboot. Open drivers can be rebound with the
display server running.

## Second power cut, with the freeze (7 October 2026, 13:55)

Fresh boot, driver patch revision 4, kernel module with the freeze hook. A dry
run first, with the card powered: `freeze` then `thaw` through
`/proc/driver/nvidia/zss_hold` both returned at once and `nvidia-smi` worked
afterwards (`docs/track-c/freeze-dry-run.log`). Then the cut, as before, under
`zss-run vkcube` (`docs/track-c/surprise-cut-2.log`).

| After the cut | What happened |
| :--- | :--- |
| within 125 ms | The NVIDIA driver logged `Xid 79 ... GPU has fallen off the bus`, **against `vkcube` itself** |
| 125 ms | The kernel module reported the loss and froze the driver |
| under 0.9 s | `vkcube` was dead: `SIGABRT` |
| 20 s | Machine up, X answering, driver `frozen`, daemon `lost` |
| 120 s | The daemon gave up waiting for the dead `vkcube` to answer its evacuation request |
| about 150 s | The device was taken back: power on, PCI state restored, **`NVRM: ZSS: thaw done (0x0)`**, module state `on` |
| a second later | The module reported the device silent again and froze the driver a second time |
| same second | The log stops. **The machine reset itself**, with nobody touching it |

What it shows:

- **The freeze lost the race.** An application rendering flat out calls into
  the driver many times in 125 ms, and one of those calls found the device
  missing before the guard did. Polling cannot be made fast enough to win
  that; the freeze can only protect a card that is not being hammered.
- **`vkcube` did not hang this time, it crashed.** The dead driver answered a
  call with an error that the layer passed on; `vkcube` reacted by creating a
  device again, that failed, and the Vulkan loader aborted on the invalid
  handle (`vkGetDeviceQueue: Invalid device`). The layer logged nothing: it
  never saw a result it recognised as a loss.
- **The thaw reported success even though the driver had already logged
  Xid 79.** That is the first sign that this driver can be brought back in
  place. It is one line in a log, not a working GPU: nothing was run on the
  card afterwards.
- **Something then made the device go silent again and the machine reset
  itself.** The user did not reboot it. The next boot began 66 seconds after
  the last log line, which is about what the firmware, the 15-second boot menu
  and the kernel take, so the reset was immediate. There is no panic message,
  no saved crash record, and `kernel.panic` is 0 (a kernel panic would have
  halted, not rebooted); the file system replayed its log on the way up. That
  is a reset below the kernel: a fatal bus error or a triple fault, the kind
  of failure the original design's root-port shield was meant to prevent. The
  cause is not established. Two suspects: the daemon rescanned the
  whole PCI bus as part of taking a lost device back (the last kernel line is
  a bridge being resized by that rescan), and the guard may have mistaken a
  reset the driver gives the card while re-initialising it for a second loss.

Changes made after it, none yet tried on the hardware:

- Layer: any error from a driver is checked against the bus before it is
  passed to the application; if the device no longer answers, it is a loss.
- Daemon: an application that has died or is dumping core is not waited for;
  a device the kernel module took back is not followed by a bus rescan.
- Module: for five seconds after a device comes back, silence is not taken
  for a loss.

## Third power cut: GPU idle, and brought back without a reboot (7 October 2026, 15:34)

With the changes listed above installed, and nothing rendering on the card
(`docs/track-c/surprise-cut-3.log`).

| Step | Result |
| :--- | :--- |
| Rail cut unannounced | Kernel module reported the loss after 50 ms |
| Driver | Frozen at once. **No `Xid 79`**: the driver never found out |
| 25 s with the card gone | Machine and X fine; daemon `lost`, `driver=frozen` |
| `zssctl on` | Power restored, PCI state restored, `NVRM: ZSS: thaw done (0x0)`, daemon `attached` |
| Twelve seconds after | Card answering throughout, no second loss, no reset |
| `nvidia-smi` | `NVIDIA GeForce GT 650M, 46, 5 MiB` |
| `zss-run vkcube` started afterwards | Selected the NVIDIA card and rendered on it |

This is the first time the card and its driver came back from an unannounced
power cut without a reboot. It rests on the freeze landing before anything
called into the driver, which an idle card allows and a busy one, so far, does
not. The reset seen in the second run did not recur; the bus rescan it was
blamed on had been removed, which fits that blame without proving it.

## Fourth power cut: `vkcube` rendering again (7 October 2026, 15:36)

Same as the second run, with the changes made after it
(`docs/track-c/surprise-cut-4.log`).

| Step | Result |
| :--- | :--- |
| Rail cut | Module reported the loss after 99 ms |
| Driver | `Xid 79 ... GPU has fallen off the bus` against `vkcube`, then frozen: the race was lost again |
| `vkcube` | **Did not crash this time**, and did not move either: alive, asleep, still holding the NVIDIA nodes 25 s later |
| Machine and X | Fine for the 30 s of observation |
| `zssctl on` | **The machine hung.** The user rebooted it by hand |

Reading of it, not all of it proven:

- The layer change held: the dead driver's error no longer reached `vkcube`.
- `vkcube` was never rebuilt on Intel because, with the driver frozen, every
  call into it sleeps until the thaw. The thread that would have come out
  with an error after the first cut now does not come out at all. The freeze
  and the evacuation work against each other.
- The hang at `zssctl on` is the thaw resuming the driver under a client whose
  state on the card no longer exists. A real suspend saves that state and
  restores it; a freeze cannot. In the third run only the X server and
  `nvidia-persistenced` held the card, both idle, and the thaw was clean.

So, on this driver: a card lost while idle comes back without a reboot; a card
lost while a program is rendering on it does not, and trying takes the machine
down. After this run the daemon refuses to bring a lost device with a frozen
driver back while anything but the display server and the listed services
still holds it, and names what has to be closed first. Whether closing those
programs and then thawing works has not been tried.

## Not done

- **Power saving is measured once only.** Repeating it several times, and with the dGPU busy beforehand, would tighten the figure.
- **The unbind strategy** (PCI remove, power cycle, rescan) was not tried.
- **Bringing the card back after an unannounced power cut**, short of a reboot.
- **Longer off periods**, and a cycle while an application is using the dGPU
  outside the layer.
