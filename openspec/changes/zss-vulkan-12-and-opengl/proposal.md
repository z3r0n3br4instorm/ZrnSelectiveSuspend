## Why

The graphics layer is what lets an application move between GPUs and outlive the loss of one, and it speaks Vulkan 1.0 with swapchains and nothing else. That covers `vkcube` and the project's own test program. It does not cover anything a person actually uses:

- **Real Vulkan applications ask for more.** Zed runs accelerated on the reference laptop today, outside the layer; under the layer it gets a Vulkan 1.0 device and cannot use it.
- **OpenGL applications do not pass through the layer at all.** `glxgears` on the NVIDIA card was frozen across a power cut and thawed, which is the fallback working, but it was not moved, and it came back with wrong colours. A browser is in the same position.
- **Everything outside the layer weakens the rest of the project.** It is what has to be frozen on `zssctl off`, what cannot be evacuated when a card is lost, and what the "all programs under the layer" idea is waiting for.

Both GPUs in the reference laptop offer Vulkan 1.2 (Intel through Mesa, with support Mesa calls incomplete; NVIDIA 470), and Mesa's Zink driver, which implements OpenGL on top of Vulkan, is installed. So one piece of work reaches both targets: bring the layer to Vulkan 1.2 with the extensions real applications and Zink use, and run OpenGL programs on Zink over the layer.

## What Changes

- **The layer presents Vulkan 1.2**, and a defined set of extensions, instead of 1.0. Every command it presents is either fully tracked, so the application stays migratable, or its use is recorded as making that application non-migratable, as today.
- **The layer's command handling is generated** from the Vulkan registry (`vk.xml`) instead of written by hand, with hand-written code kept for the objects that hold state. Vulkan 1.0 is 137 commands and the layer handles 127 by hand; 1.2 is 178, before extensions.
- **A survey tool** records what a real application asks of Vulkan (version, extensions, features, commands, structures), run against the real drivers. The extension set and the order of work come from surveys of the target applications, not from guesses.
- **A portable profile.** By default the layer offers an application only what every GPU it could be moved to supports, so that using a feature never strands it. Each GPU's own full capability remains available on request.
- **New kinds of state are tracked**: descriptor update templates, render pass 2, timeline semaphores, sampler conversions, query pools, events and buffer views (the last three are forwarded untracked today and make a device non-migratable).
- **Buffer device addresses** are offered, and their use marks the application as movable only to a GPU where the same addresses can be reproduced, since the addresses live inside the application's own data.
- **OpenGL through Zink**: `zss-run --gl PROGRAM` runs an OpenGL or OpenGL ES program on Mesa's Zink driver over the layer, so it migrates and recovers like a Vulkan one.
- **Named applications as the measure**: `vkcube` at 1.2, `glxgears` and `glmark2` through Zink, Zed, and Firefox, each started under the layer, moved between GPUs, and moved back.
- **A frozen program keeps its picture.** For programs that still cannot run under the layer, the NVIDIA driver is configured to preserve video memory across the suspend used for power-off.

Out of scope: Vulkan 1.3 and later (neither real GPU here offers it), ray tracing, video decode, Wayland compositors as clients, Proton and DXVK, device groups, and performance tuning beyond "usable".

## Capabilities

### New Capabilities

- `vulkan-coverage`: what Vulkan the layer presents, how it decides what to offer, and what happens to an application that uses something the layer cannot move.
- `opengl-through-zink`: running OpenGL and OpenGL ES programs under the layer by way of Zink.
- `application-compatibility`: the survey of what applications need, the named applications that must work, and what is done for programs that still run outside the layer.

### Modified Capabilities

None. No specification has been archived yet. Behaviour for Vulkan 1.0 applications is unchanged.

## Impact

- **New code**: `tools/` (the generator, its command classification, the survey layer), generated sources under the build directory, new tracked object kinds in `src/layer/`.
- **Changed code**: most of `src/layer/` (hand-written forwarding replaced by generated code; `cmd.c` recording becomes generated), `zss-run`, `tests/testapp` and the migration tests, `packaging/` (an NVIDIA module option), README.
- **Build**: Python and the Vulkan registry (already fetched with the headers) become build requirements.
- **Risk**: this rewrites the mechanism every existing test depends on. The first milestone is the generated code reproducing today's 1.0 behaviour with every existing test passing, before anything is added.
- **Hardware limits**: Intel's Vulkan on this laptop's GPU is incomplete, so some applications may run under the layer and still have nowhere to move but the software renderer. That is measured early and reported, not assumed away.
- **Not affected**: the daemon, the kernel module, the driver patch.
