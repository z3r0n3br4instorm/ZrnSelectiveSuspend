# Ideas proposed during the ZSS sessions

A record of the ideas the project's author put forward while ZrnSelectiveSuspend
was being designed and built (5 to 7 October 2026), in the order they came up,
with what became of each. The wording under "As proposed" is the author's own,
lightly trimmed.

Status words: **built** (exists and tested somewhere), **built, unproven**
(exists, not yet tested against the case it is for), **partly built**,
**changed** (built in a different form), **dropped**, **open**.

| # | Idea | Status |
| :-- | :--- | :--- |
| 1 | [Universal, and the device itself hot-pluggable](ideas.md#1-universal-and-the-device-itself-hot-pluggable) | partly built |
| 2 | [Surprise removal: freeze the processes, resume on another GPU, `ZSSFailedResumeNoDRM`](ideas.md#2-surprise-removal-freeze-resume-elsewhere-or-fail-with-a-named-error) | changed |
| 3 | [Checkpoint to disk instead of RAM](ideas.md#3-checkpoint-to-disk-instead-of-ram) | built |
| 4 | [Happy path first, QEMU before the laptop](ideas.md#4-happy-path-first-qemu-before-the-laptop) | built |
| 5 | [Suspend the driver instead of unbinding it](ideas.md#5-suspend-the-driver-instead-of-unbinding-it) | built |
| 6 | [Power the GPU off and on without leaving the desktop](ideas.md#6-power-off-and-on-without-leaving-the-desktop) | built |
| 7 | [No workarounds: override the NVIDIA driver's wait](ideas.md#7-no-workarounds-override-the-nvidia-drivers-wait) | built |
| 8 | [Kernel module, service, helpers and an installer for other machines](ideas.md#8-kernel-module-service-helpers-and-an-installer) | built |
| 9 | [Named progress lines while a GPU is disconnected](ideas.md#9-named-progress-lines) | built |
| 10 | [`zssctl off` freezes a process it cannot move](ideas.md#10-zssctl-off-freezes-what-it-cannot-move) | built |
| 11 | [Every program starts under the ZSS layer](ideas.md#11-every-program-under-the-zss-layer) | open |
| 12 | [A GPU switched off by command stays off](ideas.md#12-switched-off-by-command-stays-off) | built |
| 13 | [Be able to prove the GPU is really off](ideas.md#13-prove-the-gpu-is-really-off) | built |
| 14 | [A kernel shim, so the project can extend to other GPUs](ideas.md#14-a-kernel-shim-for-other-gpus) | built, unproven |
| 15 | [Virtualise the card, because cards with a video BIOS must be re-POSTed](ideas.md#15-virtualise-the-card-for-the-video-bios) | dropped |
| 16 | [Cut the gmux power rail unannounced under a running application](ideas.md#16-cut-the-power-rail-unannounced) | built |
| 17 | [A way to restore the driver and power the GPU up again](ideas.md#17-restore-the-driver-and-power-up-again) | partly built |
| 18 | [IOMMU virtualisation as the way to revive the driver](ideas.md#18-iommu-virtualisation) | dropped |
| 19 | [Freeze the driver the moment the card disappears](ideas.md#19-freeze-the-driver-the-moment-the-card-disappears) | partly built |
| 20 | [Keep this log](ideas.md#20-keep-this-log) | built |
| 21 | [A skill that keeps the log by itself](ideas.md#21-a-skill-that-keeps-the-log-by-itself) | built, unproven |

Standing constraints the author set along the way, which shaped several of the
answers: external displays must keep working (so the NVIDIA driver stays loaded
in X); the bootloader's configuration is not to be touched; and the laptop is a
work machine that must not be left unbootable.
