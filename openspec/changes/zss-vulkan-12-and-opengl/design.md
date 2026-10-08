## Context

See `proposal.md`. Facts checked before writing this:

- The layer is a Vulkan driver (ICD) in user space, about 6,900 lines, with 127 hand-written entry points. It owns every handle the application sees, records command buffers and replays them, shadows mapped memory, and rebuilds everything on another GPU on request or after a loss. It advertises Vulkan 1.0 and `VK_KHR_swapchain`.
- The registry (`subprojects/Vulkan-Headers-1.4.346/registry/vk.xml`) lists 137 commands for Vulkan 1.0, 28 more for 1.1 and 13 more for 1.2: 178 in all, before extensions.
- On the reference laptop: Intel HD 4000 reports Vulkan 1.2.354 (Mesa `hasvk`, "Ivy Bridge Vulkan support is incomplete"), the NVIDIA GT 650M reports 1.2.175, llvmpipe 1.4.354. Zink is installed. Zed, Firefox and Chromium are installed, and Zed runs accelerated outside the layer.
- Three kinds of object (events, query pools, buffer views) and some pipeline features are forwarded untracked today; using one makes the device non-migratable.
- What Zed, Firefox and Zink actually ask of Vulkan has **not** been measured. Lists in this document that depend on it are marked as to be confirmed by the survey.

## Goals / Non-Goals

**Goals:**

- A real application (Zed, Firefox) and ordinary OpenGL programs start under the layer, render correctly, move to another GPU and back, and survive an injected loss.
- Vulkan 1.0 applications behave exactly as before; every existing test passes at every step.
- Adding an extension later is a line in a table and, at most, one hand-written object kind, not a new file of forwarding code.
- An application is never offered something that would silently strand it.

**Non-Goals:**

- Vulkan 1.3 or later, ray tracing, video, mesh shaders.
- Passing the Vulkan conformance suite. A subset is used as a smoke test where it is installed.
- Matching native performance. The target is that the applications are usable.
- A hand-written OpenGL implementation.

## Decisions

### D1. Generate the layer from the registry

A generator (`tools/zss_gen.py`, run by Meson) reads `vk.xml` and a classification table and writes:

- the entry-point table and the per-driver dispatch tables;
- for every structure: a deep copy that follows `pNext` chains and array members, and a walker that translates the handles inside it;
- for every command classified as *forwarded*: its whole body (translate handles, call the driver, apply the retry-on-loss wrapper);
- for every `vkCmd*` command: a recording operation (deep copy of its arguments into the command buffer's list) and its replay.

Hand-written code remains for what holds state or contents: instances and devices, memory and its shadow, buffers and images and their contents, descriptor pools and sets, command pools and buffers, swapchains and surfaces, fences and semaphores, pipelines and pipeline caches, and migration itself.

The classification table (`tools/commands.toml`) gives each command one of:

| Class | Meaning |
| :--- | :--- |
| `forward` | stateless on our side: generated completely |
| `record` | a command-buffer command: generated recording and replay |
| `object` | creates, destroys or changes a tracked object: generated shell, hand-written hook |
| `manual` | hand-written entirely |
| `pin` | offered, but using it makes the application non-migratable |
| `absent` | not offered |

A command in the registry for an offered version or extension that is missing from the table fails the build. Nothing is offered by accident.

*Alternative considered:* keep writing by hand. Rejected: 51 more core commands, a few hundred structures with `pNext` chains, and every later extension, each a chance to mistranslate a handle.

*Alternative considered:* become a Vulkan layer above the real drivers instead of a driver. Rejected long ago for the same reason as now: a layer does not own the handles, so it cannot swap the device underneath a running application.

### D2. Parity first

The first milestone changes no behaviour: the generated code replaces the hand-written forwarding and recording for Vulkan 1.0, the advertised version stays 1.0, and all ten test suites pass, with exact-frame comparison, before any command is added. Only then does the version move.

### D3. Survey before building

`zss-survey PROGRAM` runs a program on the real drivers with a generated observation layer (an ordinary Vulkan layer, since observing does not need to own handles) and writes a report: API version requested, instance and device extensions enabled, features enabled, every command called with a count, every structure type seen in a `pNext` chain. Surveys of `vkcube`, Zed, Firefox on Zink, `glmark2` on Zink and Chromium decide which extensions are offered and in what order. The reports are kept in `docs/surveys/`.

### D4. What is offered: the portable profile

An application that uses a feature only one GPU has cannot leave that GPU. Today that is found out at migration time. With real applications it has to be prevented at the start, because they enable whatever is offered.

- **Portable (default).** Each physical device the layer presents reports the version, features, limits, formats and extensions common to every GPU in its *group*: the GPUs the daemon may move its applications to. Limits take the weaker value. The software renderer joins the group only when software fallback is allowed.
- **Native** (`ZSS_PROFILE=native`). Each device reports what its own driver offers, as now, and migration is refused later if the target lacks something in use.

If the portable profile of a group is too poor for an application to start (likely with this laptop's Intel GPU), the layer says which GPU and which missing capability caused it, so the choice between native and a smaller group is informed.

### D5. New tracked state

| Object or feature | Kept | On rebuild |
| :--- | :--- | :--- |
| Descriptor update template | creation parameters | recreated; updates through it are applied to the tracked set as ordinary writes |
| Render pass 2, imageless framebuffer | creation parameters, attachments at begin | recreated; recorded in the command stream |
| Sampler Y′CbCr conversion | creation parameters | recreated |
| Timeline semaphore | last value known to be signalled | recreated with that value as its initial value |
| Query pool | creation parameters; results are not carried | recreated empty; a query that was active is reported unavailable |
| Event | creation parameters and state | recreated in the same state |
| Buffer view | creation parameters | recreated |
| Descriptor indexing | sets as today, including partially bound and variable counts | recreated |

Structures that only extend existing create calls (most of 1.1 and 1.2) need no object of their own: the generated deep copy keeps their `pNext` chains, and the capability check compares the features they enable.

### D6. Buffer device addresses

An application that asks for a buffer's address stores it in its own data, where the layer cannot find or change it. So:

- the feature is offered;
- the layer allocates such buffers with capture and replay where the driver supports it, recording the address;
- once an application has asked for an address, it is *pinned*: it can be rebuilt only on a GPU and driver that can give back the same addresses (in practice the same GPU after a reset or power cycle), and is otherwise treated as non-migratable, which today means frozen for a power-off and unrecoverable after a loss elsewhere.

The status output says why an application is pinned.

### D7. OpenGL through Zink

`zss-run --gl PROGRAM` sets the environment so that GLX and EGL resolve to Mesa, Mesa's driver is Zink, and Zink's Vulkan is the layer. The program is then a Vulkan application as far as ZSS is concerned, with Zink's swapchains going through the layer's.

Which OpenGL version results depends on what the layer offers Zink. The target is desktop OpenGL 3.3 and OpenGL ES 3.0, which is what Firefox's renderer needs; the exact extension list is taken from the survey of Zink and confirmed against Mesa's documentation.

Whether Zink runs at all on this laptop's Intel driver is unknown. If it does not, OpenGL programs under the layer can live on the NVIDIA card and the software renderer only, and the document will say so.

*Alternative considered:* an OpenGL shim of our own. Rejected: OpenGL's state is far larger than Vulkan's and Zink already is that translation, maintained by Mesa.

*Alternative considered:* ANGLE for browsers. Kept as a note: Chromium can use ANGLE's Vulkan backend directly, which makes it a plain Vulkan application for the layer; it is surveyed along with the rest.

### D8. Programs that still run outside the layer

Freezing remains the fallback. `glxgears` came back from a power cut with wrong colours, and the NVIDIA driver is running with `PreserveVideoMemoryAllocations` off, which leaves what a program had in video memory unsaved across the suspend ZSS uses. The installer sets that option (and the temporary path it needs) for the NVIDIA driver, and the result is checked on the laptop with the same program. If the colours are still wrong the option is not the cause and the task stays open.

### D9. Testing

| What | How |
| :--- | :--- |
| Parity | all existing suites, unchanged, after the generator replaces the hand-written code |
| Each new tracked kind | the test program gains a scene that uses it; exact-frame migration and injected-loss tests as today, between NVIDIA, Intel and llvmpipe |
| The classification table | build fails on an unclassified command; a test starts a device with every offered extension enabled |
| Portable profile | a test with a fake group of two drivers of different capability checks that only the common part is reported |
| Vulkan smoke | a fixed subset of the conformance suite where it is installed; skipped otherwise |
| Zink | `glxgears` and `glmark2` scenes under `zss-run --gl`: render, migrate, compare screenshots within tolerance |
| Applications | Zed and Firefox: start, show content, migrate away and back, survive an injected loss; recorded by hand with screenshots in `docs/` |

## Findings from the surveys (7 October 2026)

`docs/surveys/README.md` has the detail. What changes the plan:

- **Zink does not load on the NVIDIA 470 driver** because Zink in Mesa 26.2 requires `VK_KHR_maintenance5` and `VK_KHR_dynamic_rendering`, and that driver has neither and never will. Proven by making a layer pretend they exist: Zink then proceeds to create a device. So D7 as written delivers OpenGL programs that can live on the Intel GPU and the software renderer only. The way round is for the ZSS layer to offer those two extensions itself on drivers that lack them, lowering dynamic rendering to render passes and framebuffers when it replays commands, which is work of its own and is not in the tasks yet (task 1.7 is the decision).
- **The NVIDIA 470 driver has no capture and replay of buffer addresses.** D6 assumed a pinned application could at least be rebuilt on the same GPU after a reset; on this driver it cannot.
- **Zink over GLX needs `LIBGL_KOPPER_DRI2=1` here**, and reaches OpenGL 3.2 on the Intel driver, not the 3.3 that was the target.
- **Zink enables whatever the driver offers**, so a survey of Zink shows what a driver has, not what Zink needs. Its requirements for a given OpenGL version come from Mesa's documentation and from trying.
- **Chromium on ANGLE is nine stateless Vulkan 1.1 commands away** on the command list, and enables external memory by file descriptor and dma-buf. External memory moves from an open question to something the first real application needs an answer for.
- **Firefox was not on Zink** with the environment used; and **Zed could not be surveyed** in a second instance.

## Risks / Trade-offs

- **The rewrite breaks what works** → parity milestone first; no feature work until every suite passes on generated code.
- **Intel's incomplete Vulkan makes the portable profile too small for real applications** → measured in the first group of tasks; native profile and software fallback are the answers, and the limits are documented.
- **Real applications use things the layer cannot move** (external memory and dma-buf sharing, buffer device addresses) → offered and pinned, or not offered so the application takes its fallback path; decided per item from the surveys.
- **Recording every command with a deep copy costs time and memory** → measured on the applications; arena allocation per command buffer if it shows.
- **Retention grows with real applications' textures** → the store's existing size cap applies; measured on Firefox.
- **The NVIDIA 470 driver is old** (1.2.175) → the offered extension set is the intersection, so it cannot exceed what this driver has; applications that need more are reported by the survey before any work is done for them.
- **Zink may not run on one of the GPUs** → see D7.

## Findings from running Chromium (8 October 2026)

`docs/applications/chromium.md` has the detail. What changes the plan:

- **Order of work.** The milestones said parity on generated code first, then 1.1. What was done instead is 1.1 by hand on generated tables (features, chained structures, limits), because a real application was within reach and each thing it needed was small. Group 2 is still wanted before the command list grows much further; nothing done since makes it harder.
- **The portable profile had to cover more than the plan listed**: queue families (Chromium asks for as many queues as the family has; the Intel GPU has one) and what a window's surface allows (image counts, formats, present modes, uses).
- **The profile of NVIDIA and Intel is enough for Chromium**, but only because all three drivers have `VK_EXT_transform_feedback`: without the `vertexPipelineStoresAndAtomics` feature, which the Intel GPU lacks, ANGLE needs that extension to offer OpenGL ES 3.0, and Chromium refuses to start on less.
- **"Out of date" is not something every application survives.** D5 kept the swapchain as the one object the application rebuilds itself. Chromium treats that answer as fatal and restarts its GPU process. The swapchain is now rebuilt in place by the layer, and the old way is the fallback.
- **Drivers disagree on a window's alpha mode.** For a window with an alpha channel NVIDIA offers "opaque" and Mesa only "inherit" and "pre-multiplied"; there is none in common. On a rebuild the layer takes "inherit" where the application's mode is missing, which is what Chromium picks for itself on Mesa.
- **Reading a window's images back** is outside what Vulkan promises (they need not keep what was presented). Drivers keep it and browsers rely on it, so the layer carries it. Writing it into the new images is done only once the application has acquired each one, which is within the rules.
- **Chromium keeps two devices**: ANGLE's and its compositor's. Only the compositor presents. Left to choose, the compositor takes the integrated GPU, so a browser "on the NVIDIA card" needs `zss-run --on`.

## Migration Plan

Each milestone is releasable: parity (no visible change), then Vulkan 1.1, then 1.2 with the surveyed extensions, then `--gl`, then the applications. `zss-run PROGRAM` keeps working throughout. Rollback at any point is the previous build.

## Open Questions

- What do Zed, Firefox, Zink and Chromium actually ask for? Answered by the surveys, first.
- Is the portable profile of NVIDIA 470 and Intel `hasvk` enough for any of them? If not, is the software renderer an acceptable place to move a browser to?
- Should external memory be offered and pinned, or withheld? Browsers use it for video and for sharing with the compositor.
- Does capture and replay of buffer addresses work on NVIDIA 470 at all?
