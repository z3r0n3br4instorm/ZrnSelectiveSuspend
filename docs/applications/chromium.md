# Chromium under the layer

Tried on 8 October 2026 with Chromium 152 on the reference laptop (NVIDIA
GT 650M on the 470 driver, Intel HD 4000 on Mesa's `hasvk`, X11).

## What works

Chromium starts on the NVIDIA card under the layer, draws at the display's
rate, is moved to the Intel GPU and back while it runs, and keeps the same
GPU process throughout.

```sh
zss-run chromium
```

| Step | Frames presented | Capture |
| :--- | :--- | :--- |
| Running on NVIDIA | 233 after 10 s | [`chromium-1-nvidia.png`](chromium-1-nvidia.png) |
| `zssctl detach 0000:01:00.0 --to 0000:00:02.0` | 478, then 748 three seconds later | [`chromium-2-intel.png`](chromium-2-intel.png) |
| `zssctl attach 0000:01:00.0` | 989, then 1183 three seconds later | [`chromium-3-nvidia-again.png`](chromium-3-nvidia-again.png) |

The page was a canvas animation with a frame counter. The captures show the
whole window drawn correctly on each GPU, including the part of the page that
does not change from frame to frame.

**How it was run:** against a private daemon with the `dry-run` backend, so
the applications were really moved between the two GPUs but the NVIDIA
card's power and driver were not touched. A real `zssctl off` and `on` with
Chromium running has not been tried yet. Neither has a loss.

## The two flags

`zss-run` adds them by itself to any program built on Chromium, which it
recognises by the runtime files beside the binary (following launcher
scripts to find it). Seen so far on this machine: `chromium`,
`helium-browser`, `code`, `teams-for-linux`. Only `chromium` has been run
this way; the others were only recognised.

Without them a Chromium program draws through OpenGL. Started with
`prime-run` on this laptop that fails outright ("eglInitialize: Invalid
visual ID requested": the NVIDIA driver, as the secondary GPU, has no OpenGL
setup for the window format Chromium picks) and the program falls back to
software rendering.

- `--use-angle=vulkan` makes ANGLE, which gives Chromium its OpenGL ES, draw
  through Vulkan.
- `--enable-features=Vulkan` makes Chromium's compositor draw and present
  through Vulkan as well.

Without either, Chromium draws through OpenGL and creates no Vulkan device:
the layer is not involved at all, and the browser is not movable.

With the first alone, ANGLE has to present to the window itself, and its
Vulkan side offers no configuration for the kind of window Chromium makes
(one with an alpha channel). Chromium then reports "No suitable EGL configs
found". That is so with or without the layer.

## What Chromium needed of the layer

| Need | Why | In the layer |
| :--- | :--- | :--- |
| Vulkan 1.1 | ANGLE refuses less | `vk11.c` |
| `VK_KHR_image_format_list` | ANGLE refuses every configuration without it | offered; the list is kept with the image |
| `VK_EXT_provoking_vertex` | ANGLE's condition for OpenGL ES 3.0 | offered |
| `VK_EXT_transform_feedback` | the other condition for ES 3.0 on a GPU without `vertexPipelineStoresAndAtomics` (the Intel one) | offered; its commands are recorded and replayed |
| `samplerYcbcrConversion` | the compositor refuses a device without it | conversions are tracked objects; formats of more than one plane are reported as unsupported |
| No more queues than every GPU has | ANGLE takes several if the family has them; the Intel GPU has one | queue families are part of the portable profile |
| A swapchain that survives the move | Chromium treats "out of date" as a lost context and restarts its GPU process | rebuilt in place, see `src/layer/swapchain.c` |
| The window's images keep their contents | Chromium redraws only what changed | read out before the move, written in as each new image is first acquired |
| The driver stays loaded | Chromium unloads its Vulkan driver between probes | the layer is linked so that it cannot be unloaded |

## Known limits

- **Starting GPU.** `zss-run` lists only the dedicated GPU to the program.
  With `--on any`, Chromium's compositor picks the Intel GPU and only ANGLE's
  device is on the NVIDIA card.
- **Alpha.** The NVIDIA driver presents Chromium's window as opaque; Mesa has
  no such mode for that window, so after a move the window system decides.
  Nothing looked different in the captures. It would where a page leaves the
  window partly transparent.
- **Loss.** With a loss injected by the layer's test hook (the GPU "dies" at
  a chosen submit) the layer rebuilt Chromium's devices in about 0.3 s, the
  same GPU process carried on, and the window was repainted in full. A real
  power cut under the running browser was tried three times on 8 October
  2026. The first two ended with the desktop stopped and a restart: the kernel
  module froze the NVIDIA driver at the loss, and the X server slept inside
  it. Since then the driver is frozen only while the card is idle. The third
  cut (`docs/track-c/cut-chromium-2.log`): the X server paused for about 5 s
  and carried on, the layer rebuilt the browser's devices on the Intel GPU,
  and the browser kept drawing
  ([`chromium-4-after-power-cut.png`](chromium-4-after-power-cut.png)). Its
  GPU process was nevertheless killed by Chromium's own watchdog about 20 s
  after the cut and replaced: a thread stayed inside NVIDIA's library. The
  card stays unusable until the display server is restarted.
- **Video.** Formats of more than one plane are withheld, so video decoded on
  the GPU will take whatever other path Chromium has.
- **Features withheld.** Under the portable profile of these two GPUs seven
  core features of the NVIDIA card are not offered (among them 64-bit and
  16-bit integers in shaders). `ZSS_PROFILE=native` offers them, and then
  Chromium cannot be moved to the Intel GPU: it is parked instead.
