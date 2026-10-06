## Why

`zss-happy-path` handles a GPU that leaves when asked. When a GPU leaves without being asked, the system is wrong in three ways, each measured in the QEMU guest: the daemon keeps reporting the device as `attached`; its check for processes holding the device goes blind because it looks them up through sysfs entries that no longer exist; and a later `detach` reports a card that is already gone as "safe to remove". On a real GPU the application would also receive `VK_ERROR_DEVICE_LOST`, which the layer passes straight through, so the application dies.

Most of what recovery needs already exists: the layer keeps every object's creation parameters, command recordings, descriptor contents and the application's mapped data. What is missing is noticing the loss, rebuilding without reading anything back from the dead device, and keeping a copy of texture uploads.

## What Changes

- The daemon notices a managed GPU vanishing without a detach, moves it to a new `lost` state, announces that as an event, and tells every application using it to evacuate.
- The daemon remembers each GPU's device nodes while it is attached, so it can still find processes holding stale handles after the device is gone.
- A `detach` of a lost device is refused; a lost device is never reported as safe to remove. When it returns it is attached like any returning device.
- The layer treats `VK_ERROR_DEVICE_LOST` from a real driver, and the daemon's evacuate request, as the same event: it holds the application, rebuilds every object on another GPU (or the same one, after a reset) from what it holds in memory, re-issues the work that was in flight, and lets the application continue. With nowhere to go the application parks and resumes when a GPU returns.
- The layer retains a copy of data uploaded into images and device-local buffers, in a content-addressed store on disk that persists across runs, so textures survive a loss. Writes happen off the application's thread, and data already in the store is not written again.
- Contents the GPU generated itself cannot be recovered. After a loss such images and buffers come back zeroed, and the layer reports that it happened.
- A thread stuck inside a dead driver no longer blocks recovery: after a grace period the dead device is abandoned instead of torn down.
- Test aids: a switch that makes the layer behave as if the real device died at a chosen submit, and an option in the test application that reproduces the expected post-loss picture, so recovery is checked by exact frame comparison on the host.

Not in this change: true electrical surprise removal on real hardware (QEMU cannot produce it; it belongs with the gated laptop stage), any kernel code, periodic checkpointing of GPU-generated contents, and lifting the Vulkan 1.0 limit.

## Capabilities

### New Capabilities

- `device-loss-detection`: the daemon noticing an unrequested disappearance, the `lost` state, stale-handle discovery, and evacuation requests.
- `device-loss-recovery`: the layer rebuilding an application's graphics state after its device is lost, without reading from that device.
- `upload-retention`: keeping uploaded image and buffer data so it can be restored after a loss.

### Modified Capabilities

None. This change builds on `zss-happy-path`, which is not archived yet, so no main specs exist to modify. Its additions are written as new capabilities so the two changes can be archived in either order. Where behaviour introduced by `zss-happy-path` gains a case (the device states, the meaning of "safe to remove"), the new requirement states the addition in full.

## Impact

- **Code:** `src/daemon/` (loss detection, `lost` state, node cache, evacuate), `src/layer/` (loss handling in every entry point that can report it, rebuild without readback, retention store, gate timeout, fault injection), `src/zssctl/` (shows the new state), `tests/` (test application option, host and guest scenarios), `docs/protocol.md`.
- **Protocol:** two new messages, `lost` (layer to daemon) and `evacuate` (daemon to layer). Older layers ignore `evacuate`; older daemons ignore `lost`.
- **Disk:** the retention store lives under the user's cache directory with a size cap. First launch of an application writes its uploads once; later launches find them already there.
- **Runtime cost:** hashing every upload, and a bounded queue of pending writes held in memory.
- **Depends on:** `zss-happy-path` code as merged. Nothing here needs root on the host; the guest tests run as root inside QEMU as before.
