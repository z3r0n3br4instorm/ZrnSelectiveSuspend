## 1. Survey

- [x] 1.1 Write the generator's registry reader: versions, extensions, commands, structures, handle types, `pNext` relations
- [x] 1.2 Generate the survey layer and write `zss-survey`; report format as in the design
- [ ] 1.3 Survey `vkcube`, Zed, `glxgears` and `glmark2` on Zink, Firefox on Zink, and Chromium; keep the reports in `docs/surveys/` (done: `vkcube`, Chromium, `glxgears` on Zink, Firefox. Not done: Zed, which refuses to start a second instance; `glmark2`, which is not installed. Firefox did not render through Zink)
- [ ] 1.4 From the surveys, fix the list of extensions and features to offer, and record what each application needs that neither real GPU has (first findings in `docs/surveys/README.md`; the list is not fixed, because Zink enables whatever it finds and its real requirements have to come from elsewhere)
- [x] 1.5 Compute the portable profile of the laptop's NVIDIA and Intel drivers and say which of the applications it can carry (extensions only: 75 in common; Chromium would be offered 33 of its 37. Features and limits not compared yet)
- [x] 1.6 Find out why Zink does not load on the NVIDIA driver (it requires `VK_KHR_maintenance5` and `VK_KHR_dynamic_rendering`, which the 470 driver lacks; proven by pretending them. See `docs/surveys/README.md`)
- [ ] 1.7 Decide: the layer supplies dynamic rendering and maintenance5 on drivers that lack them, or OpenGL through Zink stays off the NVIDIA card

## 2. Parity on generated code

- [ ] 2.1 Write the classification table for Vulkan 1.0 and `VK_KHR_swapchain`; fail the build on an unclassified command
- [ ] 2.2 Generate structure deep copy and handle translation; replace the hand-written copies
- [ ] 2.3 Generate the `forward` commands; remove their hand-written bodies
- [ ] 2.4 Generate command recording and replay; replace `cmd.c`'s hand-written operations
- [ ] 2.5 Run all ten suites; frames identical to before; no change in what is advertised

## 3. Vulkan 1.1

- [x] 3.1 Classify the 1.1 commands; add `GetPhysicalDevice*2` with generated chain handling (written by hand in `src/layer/vk11.c` on tables generated from the registry by `tools/gen_layer_tables.py`; group 2's fully generated code was not done first. See "Order of work" in the design)
- [x] 3.2 Descriptor update templates and sampler conversions as tracked objects (a conversion is a tracked object, recreated on a rebuild; a template is unpacked into ordinary tracked writes. Images of more than one plane are not carried across, so those formats are reported as unsupported)
- [ ] 3.3 Queue and device creation with protected and multiview structures passed through or refused deliberately
- [ ] 3.4 Extend the test program and the migration and loss tests; advertise 1.1 (1.1 is advertised, `ZSS_VULKAN=1.0` restores the old answer, and the ten suites pass. The test program itself still uses 1.0 only)

## 4. Vulkan 1.2 and the surveyed extensions

- [ ] 4.1 Render pass 2 and imageless framebuffers
- [ ] 4.2 Timeline semaphores, with their value carried across a rebuild
- [ ] 4.3 Query pools, events and buffer views tracked instead of opaque
- [ ] 4.4 Descriptor indexing
- [ ] 4.5 Buffer device addresses: capture and replay where the driver has it, pinning, and the reason in `zssctl status`
- [ ] 4.6 The remaining extensions from 1.4, each classified (done: `VK_EXT_provoking_vertex`, `VK_KHR_image_format_list`, `VK_EXT_transform_feedback` with its commands recorded and replayed. The rest are open)
- [ ] 4.7 Extend the test program and tests for each; advertise 1.2

## 5. Portable profile

- [x] 5.1 Compute the common version, features, limits, formats and extensions of a group of GPUs (`src/layer/profile.c`; also queue families and what a window's surface allows. Not common yet: the properties structures of extensions, and per-format image limits, which are still the present GPU's)
- [x] 5.2 Report it by default; `ZSS_PROFILE=native` for each GPU's own
- [x] 5.3 Log what limited the profile when a device cannot be created under it (the feature and the GPU that lacks it are named)
- [x] 5.4 Test with two drivers of different capability (track-a: the software renderer and the Intel GPU; the old parking test now runs under the native profile)

## 6. OpenGL through Zink

- [ ] 6.1 `zss-run --gl`: environment for GLX and EGL, with a clear error when Zink is missing
- [ ] 6.2 `glxgears` under it on each GPU: renders, renderer string, listed by the daemon
- [ ] 6.3 Reach OpenGL 3.3 and OpenGL ES 3.0: add what Zink reports missing
- [ ] 6.4 Migration and injected-loss tests with screenshot comparison; `glmark2` scenes
- [ ] 6.5 Record whether Zink runs on the Intel driver, and what that means for where OpenGL programs can move

## 7. Applications

- [ ] 7.1 `vkcube` at 1.2: start, move, return, injected loss
- [ ] 7.2 Zed: start with a file, move, return, injected loss; screenshots
- [ ] 7.3 Firefox: start on animated content, move, return, injected loss; screenshots
- [ ] 7.4 Measure memory and time cost of recording and retention on Firefox; act only if it is unusable
- [ ] 7.5 For anything that does not work, record why against its survey
- [x] 7.6 Chromium (`--use-angle=vulkan --enable-features=Vulkan`): start on the NVIDIA card, move to Intel, return, with window captures (`docs/applications/chromium.md`). Done with a dry-run daemon: the applications were moved, the card's power was not touched
- [ ] 7.7 Chromium through a real `zssctl off` and `on` on the laptop, and through a loss (injected loss: survives. Real power cut: twice the desktop stopped, with the driver frozen under it; with the freeze made conditional (kernel module `on_loss`, set by the daemon) the third cut left the desktop running and the browser drawing on Intel, though Chromium replaced its GPU process about 20 s later. Real `zssctl off` and `on` not tried)

## 8. Programs outside the layer

- [ ] 8.1 Installer: set the NVIDIA driver's video-memory preservation option and its temporary path, reversibly
- [ ] 8.2 On the laptop: `glxgears` on the NVIDIA driver through a `zssctl off` and `on`; confirm the colours, or record that the option was not the cause

## 9. Documents

- [ ] 9.1 README: what runs under the layer now, `--gl`, the profiles, the application table
- [ ] 9.2 `docs/`: the generator and classification table, how to add an extension, the surveys
- [ ] 9.3 SPEC and the "what is universal" table

## 10. Found while running Chromium

- [x] 10.1 Keep the layer loaded once loaded: Chromium unloads its Vulkan driver between probes, which left two control threads and a dead daemon connection (`-z nodelete`)
- [x] 10.2 Rebuild a swapchain in place on the new GPU instead of reporting it out of date, which Chromium treats as fatal: same image count, index map between the application's numbering and the driver's, layout and contents put in each new image when the application first receives it
- [x] 10.3 Carry the contents of a window's images across a move: a browser redraws only what changed and counts on the rest still being there
- [x] 10.4 `zss-run --on GPU` (`ZSS_START_ON`): start a program on a named GPU. Chromium's compositor picks the integrated GPU when it has the choice
- [x] 10.5 The daemon logs why a program was parked
- [x] 10.8 `zss-run` starts on the dedicated GPU by default, refuses a PCI address that is not a GPU, says where it starts the program, and gives a Chromium-based program the switches that make it draw through Vulkan (`--plain`, `--print`)
- [x] 10.6 After a loss, acquire and present carry on with the rebuilt swapchain instead of answering "out of date"; the window's old swapchain is freed first when the rebuild is on the same driver; and because a lost GPU's images cannot be read back, the display server is asked to have the window repainted (X11). Tried on Chromium with an injected loss: rebuilt in about 0.3 s, same GPU process, whole window drawn. Not yet with a real power cut
- [ ] 10.7 A test of the swapchain rebuild with images outstanding and with a target that gives a different image count (today: `vkcube` in track-a and track-loss, and Chromium by hand)
