# What real applications ask of Vulkan

Surveys made on the reference laptop (NVIDIA GT 650M on driver 470.256.02,
Intel HD 4000 on Mesa 26.2 `hasvk`, llvmpipe) on 7 October 2026 with
`tools/zss-survey`, which runs a program on the real drivers under a layer
that only watches. Each report lists the version requested, the extensions
and features enabled, and every command called, and marks what the ZSS layer
does not offer today.

| Program | How it was run | GPU it chose | Asked for | Commands used | Missing from the layer |
| :--- | :--- | :--- | :--- | ---: | ---: |
| [`vkcube`](vkcube.md) | directly | NVIDIA | 1.0 | 62 | 0 |
| [Chromium](chromium-angle.md) | `--use-angle=vulkan` | NVIDIA | 1.1 | 82 | 9 |
| [`glxgears`](glxgears-zink.md) | on Zink | Intel | 1.4 | 82 | 32 |
| [Firefox](firefox-zink.md) | on Zink | Intel | 1.4 | 33 | 10 |
| Zed | | | | | not surveyed |
| `glmark2` | | | | | not installed |

## What they show

**Chromium is close.** Its nine missing commands are all Vulkan 1.1 queries:
the `...2` forms of the property, feature, format and memory-requirement
calls, and the external fence and semaphore capability queries. What it does
with them is the larger matter: it enables 37 device extensions, among them
external memory by file descriptor and dma-buf, timeline semaphores, imageless
framebuffers, render pass 2, transform feedback and synchronization2.

**Zink takes whatever is offered.** On the Intel driver it enabled 48
extensions and used the Vulkan 1.3 dynamic-state and dynamic-rendering
commands, because that driver has them. Twenty-one of those extensions do not
exist on the NVIDIA driver. So the list in its report is not a list of
requirements: Zink scales the OpenGL it offers to the Vulkan it finds. What
the layer has to offer for a given OpenGL version must be taken from Mesa's
documentation and tried, not read off this survey.

**Firefox did not render through Zink.** Its one Vulkan process made 33
different calls and no draw: a probe. The browser then drew some other way.
Getting Firefox onto Zink needs more than the environment variables used
here, and has not been worked out.

**Zed was not surveyed.** Started a second time, with its own data directory,
it prints "zed is already running" and exits, although no Zed process could
be found. To survey it, with Zed closed:

```sh
tools/zss-survey --seconds 20 --name Zed --out docs/surveys/zed.md /usr/lib/zed/zed-editor
```

## Zink on this laptop

| | Result |
| :--- | :--- |
| Zink over GLX, as shipped | fails: "DRI3 not available", although the X server lists DRI3 |
| Zink over GLX with `LIBGL_KOPPER_DRI2=1` | works on the Intel GPU: OpenGL **3.2**, not 3.3 |
| Zink over EGL | works on the Intel GPU |
| Zink on the software Vulkan driver | works: OpenGL 4.6 |
| Zink on the NVIDIA GPU, GLX or EGL | **refused, silently** |

### Why Zink refuses the NVIDIA driver

Established on 7 October 2026, with the survey layer and Mesa's source:

- Mesa's device selection does pick the NVIDIA card (`DRI_PRIME=1`), and Zink
  examines it. It never reaches `vkCreateDevice`.
- Zink in Mesa 26.2 treats five extensions as required
  (`zink_device_info.py`): `VK_KHR_maintenance1`, `VK_KHR_create_renderpass2`,
  `VK_KHR_descriptor_update_template`, **`VK_KHR_maintenance5`** and
  **`VK_KHR_dynamic_rendering`**. A device missing one is rejected with a
  message that only a debug build of Mesa prints, which is why nothing is
  said.
- The NVIDIA 470.256.02 driver has the first three and **neither of the last
  two**. Both are newer than the 470 branch, which is the last to support this
  GPU. The Intel driver and llvmpipe have all five.
- Proof: with the survey layer made to *pretend* the NVIDIA device has those
  two extensions (`ZSS_SURVEY_PRETEND`, `ZSS_SURVEY_PRETEND_FEATURE`), Zink goes
  on to call `vkCreateDevice`, and the driver refuses it for a feature it does
  not have. Pretending any other extension, or every other one, changes
  nothing.

So no setting makes current Zink run on this card's driver. The only way an
OpenGL program could reach the NVIDIA card through Zink is for something
between them to supply dynamic rendering and maintenance5 itself. The ZSS
layer is such a thing: it is the driver Zink talks to, and it records and
replays every command already. Whether that is worth building is a decision
for the plan, not a finding.

### The author's browsers

The author reports that Firefox and a Chromium-based browser started with
`prime-run` always end up on software rendering. What was checked:

- NVIDIA's own GLX and EGL both work through `prime-run`
  (`glxinfo`, `eglinfo` name the GT 650M), so the driver's side of render
  offload is sound.
- Firefox started with `prime-run`'s variables opens the NVIDIA device: its
  main process holds `/dev/nvidia0`, `/dev/nvidiactl` and the card's render
  node. Started plainly it holds the Intel ones. So Firefox does reach the
  NVIDIA driver. Whether it then composites with it or falls back was not
  read; `about:support` (the "Compositing" row and the decision log under
  it) says which, and why.
- Chromium started with `--use-angle=vulkan --enable-features=Vulkan` drew on
  the NVIDIA card with no `prime-run` at all: 1,277 presents in the survey.
  That is a way to put a Chromium-based browser on the NVIDIA card today.

## What the two real GPUs have in common

The NVIDIA driver offers 98 device extensions, the Intel driver 117, llvmpipe
185. NVIDIA and Intel share 75; all three share 71.

| Extension | NVIDIA | Intel | llvmpipe |
| :--- | :---: | :---: | :---: |
| timeline semaphore, imageless framebuffer, render pass 2 | yes | yes | yes |
| transform feedback, host query reset, synchronization2 | yes | yes | yes |
| external memory by file descriptor | yes | yes | yes |
| swapchain mutable format, provoking vertex, line rasterization | yes | yes | yes |
| buffer device address | yes | **no** | yes |
| buffer device address capture and replay | **no** | **no** | yes |
| descriptor indexing | yes | **no** | yes |
| custom border colour | yes | **no** | yes |
| dma-buf external memory, DRM format modifiers | **no** | yes | yes |
| dynamic rendering | **no** | yes | yes |
| incremental present | **no** | yes | yes |

Of the 37 extensions Chromium enabled on the NVIDIA card, four are missing on
the Intel one (custom border colour, global priority, vertex input dynamic
state, shader subgroup extended types) and none on llvmpipe. Under a portable
profile of the two real GPUs Chromium would be offered 33 of them. Whether it
starts and draws with those 33 has not been tried.

Since then (8 October 2026) Chromium has been run under the layer itself,
which offers far fewer than 33, and moved between the two GPUs: see
[`../applications/chromium.md`](../applications/chromium.md).

## What follows for the plan

1. **Vulkan 1.1's query commands come first.** They are all that stands
   between the layer and Chromium's command list, and they are stateless.
2. **External memory is not optional for a browser.** Chromium enables it;
   the layer has no notion of memory shared with another process. Offer and
   pin, or withhold and see what the browser does, has to be decided.
3. **The portable profile of the two real GPUs is usable on paper**: the
   extensions real applications lean on are mostly in the common set. Buffer
   device address and descriptor indexing are not, so anything needing them
   is tied to the NVIDIA card or the software renderer.
4. **OpenGL through Zink cannot reach the NVIDIA card as things stand**: Zink
   requires two extensions the 470 driver will never have. Either the layer
   supplies them (dynamic rendering lowered to render passes, and the handful
   of things in maintenance5), or OpenGL programs under the layer live on the
   Intel GPU and the software renderer only.
5. **OpenGL 3.3 was the target; Zink on the Intel driver reaches 3.2.**
