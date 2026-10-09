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
| 22 | [Newer Vulkan, OpenGL, and real applications](ideas.md#22-newer-vulkan-opengl-and-real-applications) | partly built |
| 23 | [The fans go to full speed when the card is off](ideas.md#23-the-fans-go-to-full-speed-when-the-card-is-off) | built |
| 24 | [Supply missing features in the shim: software, or another GPU](ideas.md#24-supply-missing-features-in-the-shim-software-or-another-gpu) | open |
| 25 | [`zss-run` means the dedicated GPU](ideas.md#25-zss-run-means-the-dedicated-gpu) | built |
| 26 | [Lie to the application about what the device supports](ideas.md#26-lie-to-the-application-about-what-the-device-supports) | open |
| 27 | [Add the browser's Vulkan switches automatically](ideas.md#27-add-the-browsers-vulkan-switches-automatically) | built |
| 28 | [Cut the power under a running browser](ideas.md#28-cut-the-power-under-a-running-browser) | tried, failed |
| 29 | [Do something about the X server on a surprise disconnect](ideas.md#29-do-something-about-the-x-server-on-a-surprise-disconnect) | built |
| 30 | [Wrap the X server in the shim too](ideas.md#30-wrap-the-x-server-in-the-shim-too) | set aside |
| 31 | [A second X server on the GPU for its display](ideas.md#31-a-second-x-server-on-the-gpu-for-its-display) | set aside |
| 32 | [A text screen when the display server goes down](ideas.md#32-a-text-screen-when-the-display-server-goes-down) | open |
| 33 | [Freeze the driver always, and divert the calls it cannot answer](ideas.md#33-freeze-the-driver-always-and-divert-the-calls-it-cannot-answer) | built, half working |
| 34 | [Programs go back to the GPU when it returns](ideas.md#34-programs-go-back-to-the-gpu-when-it-returns) | built |
| 35 | [The performance manager drives ZSS on charger changes](ideas.md#35-the-performance-manager-drives-zss-on-charger-changes) | built |
| 36 | [Route graphics programs to ZSS however they are started](ideas.md#36-route-graphics-programs-to-zss-however-they-are-started) | built |
| 37 | [Find out why the system crashed on waking](ideas.md#37-find-out-why-the-system-crashed-on-waking) | built |
| 38 | [Make IFSCL migrate](ideas.md#38-make-ifscl-migrate) | built |
| 39 | [Implement OpenGL as well](ideas.md#39-implement-opengl-as-well) | built |
| 40 | [Names for the two shims](ideas.md#40-names-for-the-two-shims) | built |
| 51 | [A surprise power cut, now that X no longer holds the card](ideas.md#51-a-surprise-power-cut-now-that-x-no-longer-holds-the-card) | built |
| 50 | [Demo videos, for the website](ideas.md#50-demo-videos-for-the-website) | built |
| 49 | [Measure the shim, then make it faster](ideas.md#49-measure-the-shim-then-make-it-faster) | built |
| 48 | [Try it on distributions other than Arch](ideas.md#48-try-it-on-distributions-other-than-arch) | built |
| 47 | [A landing that fits the screen, says "Alpha", and a wiki that warns before it hurts](ideas.md#47-a-landing-that-fits-the-screen-says-alpha-and-a-wiki-that-warns-before-it-hurts) | built |
| 46 | [A website that is also the wiki](ideas.md#46-a-website-that-is-also-the-wiki) | built |
| 45 | [Builds and releases on every push](ideas.md#45-builds-and-releases-on-every-push) | built |
| 44 | [The installer reports too](ideas.md#44-the-installer-reports-too) | built |
| 43 | [Publish the findings, and a tester kit](ideas.md#43-publish-the-findings-and-a-tester-kit) | built |
| 42 | [Present frames on the screen's GPU](ideas.md#42-present-frames-on-the-screens-gpu) | built |
| 41 | [Lend a detached GPU to a virtual machine](ideas.md#41-lend-a-detached-gpu-to-a-virtual-machine) | works on the laptop, with a real guest |

Standing constraints the author set along the way, which shaped several of the
answers: external displays must keep working (so the NVIDIA driver stays loaded
in X; lifted by the author on 9 October 2026, when the card left X for lending); the bootloader's configuration is not to be touched; and the laptop is a
work machine that must not be left unbootable.
