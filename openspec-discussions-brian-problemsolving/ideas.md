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

**Tried on the laptop by the author**, with `glxgears`: "it successfully
halted it and resumed it, but the colors were glitched". So the freeze and
thaw of a real GPU client across a real power cut works, and what the client
had in video memory does not come back intact. The NVIDIA driver is running
with `PreserveVideoMemoryAllocations` off, which is the likely reason; that
has not been confirmed by switching it on.

**Status: built.** The process survives; its picture is not yet right.

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

## 22. Newer Vulkan, OpenGL, and real applications

**As proposed.** "my next target is to get newer vulkan versions and opengl
versions work with this and run real applications under this, like a browser
or something like zed editor". It picks up idea 11 ("shouldn't all the
programs start under the zss layer ?") from the other end: make the layer
able to carry them.

**What came of it.** Not started. What was established when it was raised:
the layer offers Vulkan 1.0 today; the laptop's two real drivers both stop at
1.2 and only the software renderer goes further; and Mesa's Zink driver, which
runs OpenGL on top of Vulkan, is installed. The proposed route is therefore
one piece of work and not two: widen the layer's Vulkan to what Zink needs,
and OpenGL programs come along through Zink.

The author then corrected a doubt raised in reply ("zed works with
acceleration"): Zed runs on this laptop's Vulkan 1.2 hardware, so it is a fair
target and not out of reach. The work was written up the same day as the
change `zss-vulkan-12-and-opengl`: 40 tasks, starting with a survey of what
Zed, Firefox and Zink actually ask of Vulkan.

While the surveys were being made the author added an observation of their
own: "i could never start helium or firefox under the nvidia card with
prime-run, it will always fall back to software rendering instead of using the
accelerator", and asked for the cause of Zink refusing the NVIDIA driver to be
found first. It was: current Zink requires two Vulkan extensions the 470
driver does not have. The browser question was narrowed but not closed (the
driver's side of `prime-run` works, and Firefox does open the NVIDIA device).

Then Chromium was started on the NVIDIA card with two Vulkan flags and the
author saw it work: "holy crap acceleration works, with that flags, this means
it works right ? anyways lets implement the vulkan and opengl stuff now". It
did mean that: a Chromium-based browser draws on the card that way, with no
`prime-run`.

The implementation went by what Chromium needed rather than by the plan's
order. The layer now offers Vulkan 1.1; reports to each program only what
every GPU it may be moved to has (the "portable profile"); rebuilds a
program's swapchain on the new GPU itself, where before it told the program
to; and carries the contents of the window's images across. On 8 October 2026
Chromium, started with `zss-run --on nvidia`, was moved from the NVIDIA card
to the Intel GPU and back while drawing, with the same GPU process throughout
and correct window captures on each. That was done against a dry-run daemon:
the programs were moved, the card's power was not touched.

Not done: Vulkan 1.2, OpenGL through Zink, Zed, Firefox, and Chromium through
a real `zssctl off`.

**Status: partly built.** A browser moves between the two GPUs; the rest of
the target is open.

## 23. The fans go to full speed when the card is off

**As proposed.** "when i turn off the GPU the  fans are ramping up to full
speed", with the instruction to stop the other work and find out why.

**What came of it.** The cause was outside the project. The fan daemon,
`mbpfan`, had been patched locally to watch the card's temperature sensors
too. With the card's power cut those sensors read -127 °C, the firmware's "no
sensor", and `mbpfan` stored the reading as an unsigned number: an enormous
temperature, so full speed. It was patched to ignore such readings, rebuilt
and installed, and the fans followed the real temperature again.

They were still loud, for a real reason: the settings called for full speed
at 70 °C and the processor sat near that under a video call. The author had
the limit moved to 80.

One thing it leaves for the project: cutting a card's power makes its sensors
vanish, and other software on the machine may be reading them.

**Status: built.** The case of the card being switched off while the patched
`mbpfan` is already running has not been watched yet.

## 24. Supply missing features in the shim: software, or another GPU

**As proposed.** "this is a shim right, so we can alter these calls, on nvidia
too if there are features that are not available in for the card that a
browser is required, lets just use software for them and make them work, and
if there is another GPU that has those features, we'll route those calls only
for that GPU and route the other calls to the main GPU", said about the
features the portable profile withholds.

**What came of it.** Not built; answered with what is possible. The first
half holds: the layer sees every call and every shader, so a feature a driver
lacks can be supplied by rewriting what the application asks into what the
driver has. That is the open decision about Zink (task 1.7: the NVIDIA driver
lacks dynamic rendering, and the layer could lower it to render passes), and
it is how 64-bit and 16-bit integers in shaders could be offered on the Intel
GPU.

The second half, sending single calls to a second GPU or to software, does
not hold for drawing: a draw reads and writes the same images as the draws
around it, and two GPUs here share no memory, so every switch would copy the
frame's images through main memory. Whole programs can run on another GPU or
in software, and already do.

It also turned out that nothing was missing for the browser: the withheld
features are ones the NVIDIA card has and the Intel GPU lacks, and Chromium
ran without them.

**Status: open.** Recommended as the way to settle task 1.7; not started.

## 25. `zss-run` means the dedicated GPU

**As proposed.** "wait we dont even need the --on command, if im using zss-run
that means im using it to run a program in nvidia GPU, so it should by default
use dedicated GPU by default (nvidia, radeon)". Before it: "if the provided
address is not a GPU then error out, PCI address is not a GPU. zss-run is just
for GPUs, log that ... remembner we are currently building this for a GPU but
our long term goal is any PCI card".

**What came of it.** Built the same day. `zss-run PROGRAM` starts the program
on the discrete GPU and lists only that one to it; `--on` remains for naming
another (an address, part of a name, or "any"). An address that is not a
display controller is refused with "PCI address is not a GPU", one that does
not exist with its own message, and each run says in one line where the
program is being started and that only GPUs are managed for now. If the
dedicated GPU is powered off the program is offered the others instead.

**Status: built.**

## 26. Lie to the application about what the device supports

**As proposed.** "it will lie to the applicatiob by saying yes this device
supports what you are asking, for and then launch it, its fine if it crashes
but applications like chromium will work", offered as the way to start a
browser under `zss-run` without its two Vulkan flags, in place of the launcher
adding them.

**What came of it.** Not built; checked first. A browser started without the
flags does not ask the layer for anything it could be lied to about: Chromium
under `zss-run` with no flags created no Vulkan device at all. It draws through
OpenGL, which the layer does not see. The flags are what make it use Vulkan in
the first place.

Where lying does apply is one step further on: sending the browser's OpenGL
through Mesa's Zink, which turns it into Vulkan, and having the layer claim
the two extensions Zink insists on and the NVIDIA driver lacks. Claimed and
not backed, Zink's first drawing call would have nowhere to go; so there the
lie has to be backed by the rewrite proposed under idea 24.

**Status: open.** Depends on the OpenGL route (tasks 1.7 and 6).

## 27. Add the browser's Vulkan switches automatically

**As proposed.** After the cause of the software fallback was found: "ayyo if
thats the issue, then check what card the display is driven by, if its not
the dedicated, add those flags automatically", and then "this should be done
to chromium based applications as well, it should auto detect if the launching
applicaiton is a chromium based one, and then apply it".

**What came of it.** Built, with one change. `zss-run` recognises a program
built on Chromium by the two runtime files that sit beside its binary,
following launcher scripts and small launcher programs to find it, and adds
`--use-angle=vulkan --enable-features=Vulkan`. On this laptop it recognises
`chromium`, `helium-browser`, `code` and `teams-for-linux`, and leaves
`vkcube`, `zed` and `glxgears` alone. `zss-run chromium`, with nothing else,
was then moved from the NVIDIA card to the Intel GPU and back.

The change: the check of which card drives the display was left out. A
Chromium program without the switches draws through OpenGL, and then it is
not on the layer at all, whichever card drives the display; so under
`zss-run` the switches are always needed.

This reverses the answer to idea 26, where the author had rejected the
launcher adding the flags: once the cause was known, they asked for it.

**Status: built.** Only `chromium` itself has been run this way.

## 28. Cut the power under a running browser

**As proposed.** "okay then, the happy path works , lets try to cut power to
the GPU while a browser is running and see if it recovers".

**What came of it.** Rehearsed first, not yet done for real. With a loss
injected by the layer itself, Chromium's first two attempts ended with it
restarting its GPU process: the layer still answered "out of date" on that
path, and then could not make the new swapchain while the window's old one
existed on the same driver. Both were fixed, and the window is now asked to
repaint after a loss, since a dead GPU's images cannot be read. After that the
same GPU process carried on and the whole window was drawn, three runs out of
three.

The real cut was held back for the author's word, given as "go", and made on
8 October 2026 (`docs/track-c/cut-chromium-2026-10-08.log`). The kernel module
saw the loss in 0.1 s and froze the NVIDIA driver before it touched the dead
card, as designed. Then everything that was inside the driver went to sleep
in it: the browser's GPU threads, so the layer could not move the browser,
and the X server, which uses the NVIDIA driver for the external outputs, so
the desktop stopped. Chromium's own watchdog killed its GPU process after
about 23 s. The author reported "sadly, it crashed the X server" and pressed
the power key; the journal shows an orderly shutdown, and no crash of the X
server, which was waiting on the frozen driver.

What it shows: freezing the driver protects the kernel but holds everyone who
calls it. That is the open question of the kernel shim's task 3b.10, now
measured with a real application.

**Status: tried, failed.** The browser did not survive and the desktop had to
be restarted.

## 29. Do something about the X server on a surprise disconnect

**As proposed.** "well we need to do something about the X server crashing on
suprise disconnect", after the browser cut (idea 28) and again after a
diagnostic cut that ended in a restart ("crashed and restarted").

**What came of it.** The diagnostic cut showed where the X server was: asleep
inside the NVIDIA driver's modesetting call, because the kernel module had
frozen that driver the moment the card went. The project's own notes held the
other half: in the very first cut, before the freeze existed, the X server
had survived. The freeze protects the kernel and makes an idle card
recoverable, but with anything running on the card it stops the display
server and keeps programs from being moved.

So the freeze was made conditional. The kernel module has a switch
(`on_loss`: freeze or leave), and the daemon sets it every second from who is
using the card: leave while a program or an active output is on it, freeze
while it is idle. With that installed, the cut was repeated under the browser
(`docs/track-c/cut-chromium-2.log`): the X server paused for about five
seconds and carried on, the layer rebuilt the browser's devices on the Intel
GPU, and the browser kept drawing. No restart.

Two things it did not fix. Chromium's GPU process was still killed by its own
watchdog about 20 s after the cut and replaced by a new one: a thread stayed
inside NVIDIA's library after the loss. And the card cannot come back until
the display server is restarted, because the NVIDIA driver will not use a
device it has seen vanish; `zssctl on` now says so.

The diagnostic cut's restart was the tester's doing: the script asked the
module to take the card back with the browser still alive, which is the one
thing earlier runs had shown must not be done. The script now refuses that.

**Status: built.** The desktop survives a loss under load; a seamless browser
and the card's return without a log-out are open.

## 30. Wrap the X server in the shim too

**As proposed.** "what if we wrap x server as well ? ... we'll wrap X server
around our zss shim , and on GPU disconnect it will use some software tricks to
make the X server not crash". Followed by "will there be noticable lag".

**What came of it.** Not built; answered. The shim stands between a program
and its Vulkan driver, and the X server does not reach the card through
Vulkan: it loads NVIDIA's own X driver, which speaks a private language to the
kernel driver. There is nothing there to intercept and answer.

The aim behind it was taken up in another form: get the NVIDIA driver out of
the X server altogether. The main X server would run on the Intel GPU alone
and see the external monitor as a virtual output; a helper owned by ZSS would
show that output's frames on the card's real connector. A loss would then be
a monitor unplugged, and the card could return without a log-out. Estimated
cost: about one frame of delay on external monitors, unmeasured.

**Status: open.** Superseded in its details by idea 31.

## 31. A second X server on the GPU for its display

**As proposed.** "instead when a display connected to the GPU we'll start
another X Server running in the GPU, to that display and then connect that x
server to the main xserver so we can drag windows around, what do you think".

**What came of it.** Not built; taken as the way to do idea 30. Two X servers
cannot share windows: a window belongs to one server, and nothing in X moves
it to another. But a second X server on the card is a good helper for the
display proxy: NVIDIA's own driver sets the mode and shows the picture, so
nothing has to be proven about driving the ports without X. The main server
keeps one desktop, with the external monitor as a virtual output of it, which
is what makes dragging windows across work; the second server shows that
output full screen. If the card is lost only the second server is affected,
and bringing the card back is restarting it.

**Status: open.** Recommended; a feasibility test comes first.

## 32. A text screen when the display server goes down

**As proposed.** "wait this is a bit out of scope right, The main ZSS core unit
should be responsible for PCI-e devices hotplug, restore and keeping the system
stable ... is there a way to show a TTY screen when X crashes ? with a message",
with a draft: the subsystem detected a PCI-e or Lightridge change that affected
the display server, the session is alive, a count and list of running
programs, an attempt to get the display server back, and "Save Program states
? (Y/N)".

**What came of it.** Not built; answered. It set the scope: the second X
server and the display proxy (ideas 30 and 31) are outside the core and were
put aside. The daemon can already put progress on a text console (`zssctl off
--console`), so the screen itself is a small addition where the display
server has died or is still answering. Where it is hung, it holds the screen
and will not hand it over; then the text screen can only come after the
server is stopped, which closes the programs that have windows.

"Save program states" was answered as not possible: nothing can save a
graphical program's state from outside it. Real choices were proposed in its
place (wait, or restart the display server).

**Status: open.** Waiting for the author's choice of what the prompt offers.

## 33. Freeze the driver always, and divert the calls it cannot answer

**As proposed.** "our core should be able to handle both of this, as soon as
the card gets lost, it should handle this by freezing the driver ... if the
nvidia driver is frozen and it doesnt respond x calls, we should divert them
somehow", and with it the question "I also doesnt understand how the X server
doesnt crash when we suspend the card systematically".

**What came of it.** Not built; answered and planned. The question's answer:
in an orderly switch-off the X server does wait on the driver too, but only
for a moment, because the driver saved its state first and the daemon powers
the card back on for as long as the call takes. After a loss neither is true.

The proposal was taken as a third behaviour for the frozen driver, beside
"sleep" and "leave it to find out": stay frozen, so the driver never learns of
the loss and the card stays recoverable, but refuse callers at once instead of
putting them to sleep. Calls cannot be answered for the driver, whose language
is private; they can be turned away. Whether the X server takes a refusal in
its stride is not known, and can be found out safely with the card powered,
where a freeze is undone by a thaw.

Written the same day, on the author's "do it": revision 5 of the driver patch
(the gate refuses with an error while frozen and refusing; closing a file is
still put off until the thaw), a third value `refuse` for the kernel module's
`on_loss`, a daemon setting `loss_while_busy` that picks it, and a test that
freezes the driver in its refusing way with the card powered, asks the display
server for the things that send it into the driver, and thaws. The driver
compiles and the ten suites pass.

Installed and run once that evening, with the card powered. The display
server answered every display query at once while the driver refused (18
callers turned away, no stall), which is what the proposal needed to be true.
The thaw afterwards stopped the machine: the driver's resume decided the card
had "fallen off the bus" although it was powered. The first explanation given
(the kernel module misreading the resume as a loss) was disproved by a second
run with the module out of the way, which failed the same way; the author
reported "froze and had to restart X again". Why a refusing freeze cannot be
thawed where a plain one could the day before is not known yet.

**Status: built, half working.** Refusal does what was wanted while the card
is away. The thaw after it fails, cause unknown; the daemon's setting stays at
`leave`.
