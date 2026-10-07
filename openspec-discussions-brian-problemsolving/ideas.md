# The ideas, one by one

See [README.md](README.md) for the index and the meaning of the status words.

## 1. Universal, and the device itself hot-pluggable

**As proposed.** "the idea is not to make this only for this laptop, and its to
make the device itself hotpluggable ... when i suspends the device and
disconnectes it, the ZSS will handle the PCI disconnection, which will then
lets the user disconnect the device form the board itself". Repeated later:
"this part is only for nvidia, i mentioned this should be universal".

**What came of it.** It set the architecture: a vendor-neutral core (the Vulkan
layer, the daemon, the kernel module's sequence) with hardware-specific power
backends. Orderly detach of a removable card works in QEMU (driver unbound,
slot power cut, card ejected, card returned). On real hardware only the
MacBook's soldered NVIDIA card has been exercised.

**Status: partly built.** The README table "What is universal and what is not"
tracks it row by row. Missing: any real removable card, any driver other than
NVIDIA 470 and QEMU's `bochs`, and the `acpi` power backend on real firmware.

## 2. Surprise removal: freeze, resume elsewhere, or fail with a named error

**As proposed.** "make it work even if the card suddenly disappears, the ZSS
will emit a signal saying halt processes using this GPU ... the userspace ZSS
will detect it and freezes all those processes, if the user tries to use one of
those process, the userspace driver should try to get the graphics context to
translate for the available DRM, if no DRM available do not try to resume the
application, emmit ZSSFailedResumeNoDRM".

**What came of it.** Built as the `zss-device-loss` change, in a different
shape: an application under the layer is not frozen and resumed on demand, it
is rebuilt at once on another GPU from what the layer holds in memory, and
keeps running. Only when there is nowhere to go is it parked and frozen, and
resumed when a GPU appears; `ZSSFailedResumeNoDRM` is the error for a resume
with no usable GPU.

**Status: changed.** Rebuilding immediately turned out simpler and gives a
running application rather than a frozen one. Tested in QEMU; on the laptop the
first real power cut showed the state rebuilt but the application stuck (see
16 and 19).

## 3. Checkpoint to disk instead of RAM

**As proposed.** "instead of saving the window state checkpoint to RAM, lets
save it to disk, because the application resumes only when the user tries t
interact with it right ? its fine if it takes 10 or 20 seconds to initialize".

**What came of it.** The retention store: uploaded textures and buffers are
kept in a content-addressed store on disk (`ZSS_RETAIN=disk`, the default),
written in the background, and read back when an application is rebuilt after
a loss. RAM and off are options.

**Status: built.** Recovery takes a fraction of a second in the tests, not 10
to 20, because only what cannot be recreated is stored.

## 4. Happy path first, QEMU before the laptop

**As proposed.** "lets not focus into the suprise disconnect yet, lets first
try to get a solid ground on happy path ... lets test these stuff with a qemu
virtual machine first then lets test on this laptop".

**What came of it.** The order of the whole project: `zss-happy-path` first,
with Track A (host), Track B (QEMU, a virtual card on a hot-plug port) and
Track C (the laptop). Every later piece, the kernel module included, went
through QEMU before the laptop.

**Status: built.** It is why a kernel module could be loaded on a work machine
on its first day.

## 5. Suspend the driver instead of unbinding it

**As proposed.** "This is why i suggested suspending the driver, so when on
resume it can act as if the laptop just wakes from sleep."

**What came of it.** The suspend-in-place strategy: the driver stays bound and
is taken through its own sleep and wake, so a display server holding it does
not have to let go. It is the only route that works with X using the NVIDIA
card for external displays, and it is what the kernel module generalises to
other drivers (`quiesce=pm`).

**Status: built.** Measured on the laptop: suspend 0.05 to 0.13 s, power cut
0.03 s, resume 0.11 to 0.2 s.

## 6. Power off and on without leaving the desktop

**As proposed.** "can we turn on and off the dgpu without moving to the tty ?"
with the constraint "i need the external displays to work".

**What came of it.** First answer was no: the stock driver froze X the moment
the desktop came back with the card off. That led to idea 7. With the patched
driver the card is powered off and on under the running X session, and
`zssctl off --console` remains for the text-console route.

**Status: built.**

## 7. No workarounds: override the NVIDIA driver's wait

**As proposed.** "NO we are not making hacky solutions, we might be able to
solve this issue with the dummy driver too, the kernel module we talked about"
and "why cant we override this ? this is stupid".

**What came of it.** The wake-on-touch patch to NVIDIA's open kernel-interface
code: a caller that finds the driver suspended asks for a wake and sleeps,
instead of spinning for ever. Revisions 2 and 3 added a pollable wake file and
the list of who is waiting. It is applied through DKMS, survives kernel
updates, and rolls back at boot if the patched driver does not load.

**Status: built.** Alternatives that were turned down at this point: switching
to Nouveau, and a second X session.

## 8. Kernel module, service, helpers and an installer

**As proposed.** "shall we build the kernel module, system service and
required helpers for this to work ? and a installer too that will install this
on other computers, and patch the nvidia driver".

**What came of it.** `zssd.service`, `/etc/zss/zssd.conf`, the `zss` group,
`install.sh` and `uninstall.sh`, `zss-nvidia-patch`, a pacman hook, a boot
check, an Arch package recipe. The kernel module was judged unnecessary then
and came later (idea 14).

**Status: built.** The installer has run on one machine.

## 9. Named progress lines

**As proposed.** "when the display switches to TTY, to disconnect the GPU show
proper logs [ZrnSelectiveSuspend] Suspending device: Show some useful stuff
there, and more progress".

**What came of it.** Every power transition is reported step by step, prefixed
`[ZrnSelectiveSuspend]`, to the terminal that asked, the system log, and a
text console when one is in use.

**Status: built.**

## 10. `zssctl off` freezes what it cannot move

**As proposed.** "it should freeze that process then, not wait until it ended".

**What came of it.** A process outside the layer that holds the GPU (a monitor
such as `btop`) is frozen for the duration and thawed afterwards, instead of
blocking the power-off. Guards added: the terminal the command was typed in is
never frozen, the idle timer never freezes anything, and a plain detach still
refuses.

**Status: built.** Tested with a file-backed device and in QEMU; freezing a
real GPU client across a real power cut has not been tried.

## 11. Every program under the ZSS layer

**As proposed.** "shouldn't all the programs start under the zss layer ?"

**What came of it.** Agreed as the goal, not done: the layer offers Vulkan 1.0
only, so forcing it on every program would break those that need more, and
OpenGL programs do not pass through it at all. Hiding a switched-off GPU
(idea 12) covers part of the need by another route.

**Status: open.** Needs a wider Vulkan surface and an OpenGL path first.

## 12. Switched off by command stays off

**As proposed.** "noo it should stay off when i send a command", and later,
with evidence, "running vkcube without zss-run brought it back".

**What came of it.** Three layers, each added after the previous one proved
insufficient on the laptop: only the display server may wake a GPU that was
switched off on request; a switched-off GPU is hidden from programs started
meanwhile (device nodes, loader files, driver status files); and when X itself
needs the card it is served for a second or two and switched off again.

**Status: built.** Longest stretch off so far is about eleven minutes, ended by
a command.

## 13. Prove the GPU is really off

**As proposed.** "im not sure, i want to be dure", "lspci still shows it up",
"it is not just masking the nvidia card ... the gpu is currently fully powered
off right".

**What came of it.** The distinction between the kernel's remembered device
list and a live read of the card, written into the README: `lspci -x` showing
all `ff` means no power. A battery-draw comparison was tried and was too noisy
to count.

**Status: built.** A clean electrical measurement is still missing.

## 14. A kernel shim for other GPUs

**As proposed.** "is the dummy kernel module is installed for the suprise
disconnection ?" then "today we are gonna build the kernel shim that lets us
extend and do more with this".

**What came of it.** `zss.ko`: the driver's own sleep callbacks run for one
device, PCI state saved and restored by the kernel, power backends (`gmux`,
`acpi`, `test`), a loss guard that notices a silent card within 0.2 s, events
to the daemon, DKMS packaging. It runs on the laptop.

**Status: built, unproven** for its main purpose: no driver other than QEMU's
`bochs` has been through the sleep-callback route, and the `acpi` backend has
never run.

## 15. Virtualise the card, for the video BIOS

**As proposed.** On running the driver in a virtual machine with a fake card:
"yes but we need it because on the GPUs that has VBIOS, we need to boot the
VBIOS after suspending the card". Also the question why MMIO shadowing and DMA
isolation were left out.

**What came of it.** The need is real, the mechanism is not required: every
real GPU driver replays its card's video BIOS initialisation itself on resume,
and the laptop's card had been coming back that way all along. The module
therefore refuses to power off a card with no driver bound. MMIO shadowing was
dropped for lack of any hook, any device model for a closed driver, and any
way to test it; `SPEC.md` records the reasons.

**Status: dropped**, with the concern handled another way.

## 16. Cut the power rail unannounced

**As proposed.** "lets run vk cube on that gpu and instead of suspending it
gracefully we'll kill gmux power rail for the GPU unannounced".

**What came of it.** The first real surprise-loss test (`tests/hw_surprise_cut.py`).
The machine and X survived; the module noticed in 151 ms; the application's
state was rebuilt on Intel in 3.5 s. The author then saw what the script had
missed: the window was frozen. The cause was a thread stuck inside NVIDIA's
library waiting on the X server. Two fixes followed in the layer (cutting the
dead driver's connection to X; re-issuing work that was in flight).

A second run, with the freeze of idea 19 in place, found two more: an error
from the dead driver reached `vkcube`, which then crashed, and the daemon
waited two minutes for the dead application to answer.

**Status: built** as a test. It has found four real defects in two runs, one
of them only because the author looked at the screen.

## 17. Restore the driver and power up again

**As proposed.** "card did not came back", "we need a way to restore the driver
and power up the GPU again", "if this doesnt work that means we cant hotplug a
GPU".

**What came of it.** Powering the card and restoring its PCI state works (the
kernel module does it). Reviving NVIDIA's driver in place does not: once it has
declared the GPU lost it will not look again, and forcing its suspend path hung
in the kernel. That attempt left the machine needing a reboot. The orderly
hot-plug path does not depend on this; the unannounced one does.

**Status: partly built.** Idea 19 is the attempt to make the driver never reach
that state. The fallback is to rebind the driver once nothing holds it.

## 18. IOMMU virtualisation

**As proposed.** "but if we do the IMMOU virtualization thing will that work
then ?"

**What came of it.** Separated into three things. An IOMMU alone confines the
card's memory access and has no say over the driver. Shadowing registers only
postpones the moment the driver meets a blank card. Giving the card to a
virtual machine would work, since the driver could be restarted with the guest,
but it takes the card out of the host's X (external displays), needs a kernel
boot parameter (the bootloader), and may not be possible on this Mac.

**Status: dropped** for this machine.

## 19. Freeze the driver the moment the card disappears

**As proposed.** "what if we freezes the driver as soon as the card disappears
so it wont look for it, and when the card came back we'll resume the driver and
trigger a suspend/resume".

**What came of it.** Reading NVIDIA's source showed the idea fits the driver's
own structure: a suspend is "close the gate" then "save the card's state", and
the hang in idea 17 was in the second half. A freeze is the first half alone.
Built as driver patch revision 4 (`/proc/driver/nvidia/zss_hold`: freeze, thaw,
release), with the kernel module freezing the driver the instant its guard sees
the card go silent and thawing it, through the driver's ordinary resume, when
the card is back. The suspend step of the proposal was left out: with nothing
saved there is nothing to suspend, and that step is the one that hung.

**Tested on the laptop the same day** (the author: "lets test the suprise
disconnect shall we"). Two results pulled in opposite directions. The freeze
lost the race: `vkcube`, rendering flat out, called into the driver inside the
125 ms it took the guard to notice, and the driver logged the GPU as lost
before it was frozen. But when the card was powered again the thaw reported
success all the same, which the earlier attempt at reviving the driver (idea
17) never managed. A second later the device read as silent again and the
machine went down; why is not established.

Two more runs that afternoon settled it. With the card **idle** at the moment
of the cut, the freeze landed in 50 ms, the driver never learned the card had
gone, and `zssctl on` brought card and driver back with no reboot; `vkcube`
then rendered on it. That is the first unannounced loss this project has
recovered from completely. With `vkcube` **rendering**, the driver noticed
first again, the application neither crashed nor moved (frozen out of the
driver along with everything else), and the thaw hung the machine: the driver
was resumed under a client whose state on the card was gone.

**Status: partly built.** Proven for an idle card. For a busy card the idea
does not work on this driver as it stands, and the daemon now refuses to thaw
while such a program still holds the device. The author's instinct that the
driver must be stopped from looking was right; what it cannot do is put back
what a running program had on the card.

## 20. Keep this log

**As proposed.** "log all the ideas that i had proposed to you within this
session, in a separate folder".

**Status: built.** This folder.

## 21. A skill that keeps the log by itself

**As proposed.** "create youself a skill to log every idea that the author gave
you in the session when something is done automatically, in a folder like
this".

**What came of it.** The `idea-log` skill (`~/.claude/skills/idea-log/`), which
says what counts as an idea, how an entry is written (the author's exact
words, what came of it, a status) and how the log is brought up to date. A
standing rule in the author's global instructions applies it whenever a piece
of work is finished, in any project that has an `openspec-discussions-*`
folder at its root. Projects without such a folder are left alone until the
author asks for a log there.

**Status: built, unproven.** This entry is its first use. Whether it fires by
itself in a later session has not been seen yet.

