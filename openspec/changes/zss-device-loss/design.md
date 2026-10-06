## Context

See `proposal.md` for motivation. This builds on the code from `zss-happy-path`: the layer is a Vulkan driver shim that owns every handle and can rebuild an application's objects on another real device; `zssd` runs detach and attach.

Three experiments in the QEMU guest shape this design.

**Pulling the card with no detach** (QMP `device_del`, application running on it):

| Observed | Result |
| :--- | :--- |
| Kernel | Survived; `bochs-drm` unbound with the device node still open |
| Application's handle | `/dev/dri/card0 (deleted)`, still held |
| `zssd` state | `attached`, indefinitely |
| `zssd` holder scan | Empty: it derives device numbers from sysfs, which is gone |
| `zssctl detach` afterwards | Succeeded and reported `safe-to-remove` |

**What QMP removal is.** `device_del` presses the slot's attention button; the guest kernel waits five seconds and powers the slot off. The kernel is warned, ZSS is not. That is an *unannounced* removal, and it is what the guest can test.

**True surprise.** Setting Link Disable on the root port to drop the link under a bound driver does nothing in QEMU; the card stays. A link that just dies cannot be produced in this guest. It needs real hardware.

What the layer already holds per object, and therefore what a rebuild without readback can restore today:

| Held now | Not held |
| :--- | :--- |
| Creation parameters of every tracked object | Contents of images |
| Recorded commands of every command buffer | Contents of buffers in memory the application never mapped |
| Descriptor set contents | Anything the GPU wrote |
| A shadow of every memory object the application has mapped | |

## Goals / Non-Goals

**Goals:**

- The daemon never reports something false about a device that left by itself.
- An application under the layer survives the loss of its device with, at worst, GPU-generated contents zeroed.
- Recovery is tested by exact frame comparison on the host, with no hardware involved.
- No new dependencies in the layer.

**Non-Goals:**

- Protecting the kernel. Nothing measured so far shows the kernel needs help with an unannounced removal. If real hardware later shows a hang or panic on a true surprise, that is a separate change, and the first place kernel code may be justified.
- Recovering GPU-generated contents by checkpointing them periodically.
- Recovering applications that were not started under the layer, or that are marked non-migratable.
- Making a lost device "safe" in any sense. It is already gone.

## Decisions

### D1. One event, two detectors

A loss can be noticed first by either side, so both report it and the daemon arbitrates.

```
 device leaves bus ──► zssd: kernel uevent / poll ──┐
                                                    ├─► zssd decides target ──► "evacuate" to every client
 real driver returns DEVICE_LOST ──► layer: "lost" ─┘
```

- **Daemon:** a `NETLINK_KOBJECT_UEVENT` socket in its poll set wakes it on any PCI remove; it then checks whether each attached GPU is still present. A one-second presence poll remains as the fallback, which bounds detection at the two seconds the spec requires even if an event is missed.
- **Layer:** every entry point that can return `VK_ERROR_DEVICE_LOST` from the real driver checks for it.
- **Arbitration:** on a `lost` report the daemon checks the bus. Gone: state becomes `lost`, target chosen as for a detach. Still present: this is a driver reset, state stays `attached`, target is the same GPU. A reset may have hit only one application, so on an in-place evacuate the layer rebuilds only devices that themselves saw the loss and leaves healthy ones untouched.

The target chosen for a detach or an evacuation can be pinned with `zssd --default-target <PCI|software>`.

A managed address that has never been seen on the bus is a test device. The daemon never declares it lost by polling, treats a reported loss of it as "gone", and lets an explicit attach bring it back.

The daemon does not probe a present device's configuration space on a timer to look for a dead link: reading it wakes a runtime-suspended device. It looks only when a client reports a loss.

*Alternative considered:* let the layer pick its own target. Rejected as the normal path because two applications on one lost GPU should land consistently and the daemon already owns target policy. It remains the fallback: with no daemon, or no `evacuate` within five seconds, the layer chooses for itself.

### D2. `lost` is a state, not an error

`attached → lost` on disappearance; `lost → attaching → attached` when the device returns. Detach from `lost` is refused. `safe-to-remove` and `powered-off` are reachable only through a detach, as before.

Suspend-in-place devices (the soldered dGPU) cannot leave the bus, so for them only the client-reported path applies.

### D3. The daemon caches device numbers

`zssd` records each managed GPU's device numbers (DRM nodes, NVIDIA character device) at start-up and after every attach. After a loss it scans `/proc/*/fd` against the cache; a deleted node's open file still carries its device number. Such holders are listed with the class `stale`. The orderly path's independent "nothing still holds the device" check uses the cache too, which closes the gap that let a post-loss detach succeed.

### D4. Recovery is the existing rebuild, fed from memory

`zss_migrate` is capture → build → commit. Recovery is the same with capture replaced by "use what is held":

| Object | Source of contents on rebuild |
| :--- | :--- |
| Buffer in memory the application has ever mapped | The shadow |
| Buffer or image that received a retained upload | The retention store (D6) |
| Anything else with contents | Zeros, counted as lost |

Zero-filling is done explicitly (clear commands for images, a fill for buffers) so the result is deterministic rather than whatever the new allocation contained. The count of zeroed objects goes into the outcome message and the log.

Commit differs too: the old device is destroyed on a best-effort basis, and its driver library is still unloaded so that device files are released.

### D5. In-flight work

- A `QueueSubmit` that returned device-lost is issued again after the rebuild, with the replayed command buffers, and its result is returned to the application.
- The layer tracks which fences are pending (passed to a submit and not yet seen signalled). On rebuild after a loss, pending fences are created signalled. Semaphores keep the signalled state the layer already tracks.
- Results the GPU would have written during the lost work (a read-back buffer, for instance) are stale for that one submit. This is the unavoidable cost and is stated in the specs as "work in flight".

Every other entry point that receives device-lost waits for recovery and then repeats its real call with the new handles.

### D6. Retention store

**What is captured.** At submit time the layer walks the submitted command buffers, as it already does to track layouts. For each copy from a buffer with a shadow into an image, or into a buffer without one, it records `(destination, region, content hash)` and hands the bytes to the store. `UpdateBuffer` data is already in the recording. Lineage is one hop: a copy from a device-local buffer that itself holds a retained upload carries the record across.

**When a record dies.** When the destination is used as a render target, or is the destination of a blit, resolve, clear, or a copy from something unreadable, its records are dropped and it is marked GPU-dirty. Destroying the destination drops them too.

**Where.** `$XDG_CACHE_HOME/zss/store/<hash>`, written to a temporary name and renamed. The hash is MurmurHash3 x64-128 over the bytes, combined with the length. It is implemented in the layer (about sixty lines) to avoid a dependency. This is a cache key, not a security boundary: the store is private to the user.

**Not stalling.** The submitting thread hashes the data and checks whether the store already has it. Only new data is copied into a bounded queue (256 MB by default) drained by one writer thread. If the queue is full, the upload goes ahead unretained and its destination is marked unrecoverable.

**Trimming without breaking readers.** Each process hard-links the blobs it depends on into `store/pin-<pid>/`. The trimmer, run by the writer thread when the store passes its limit (4 GiB by default), removes least-recently-used blobs whose link count is one, and removes pin directories whose process no longer exists.

**Control.** `ZSS_RETAIN=disk` (default), `ram` (keep in memory, no files), or `off`.

*Alternatives considered:* keeping every upload in RAM (doubles texture memory for the life of the process); a synchronous write on the submit thread (stalls loading screens); a cryptographic hash (slower, and nothing here needs collision resistance against an adversary).

### D7. A stuck thread

The gate normally waits until no application thread is inside the layer. A thread blocked forever in a dead driver would make that wait endless. For loss recovery only, the gate waits a grace period (three seconds), then proceeds:

- the old real device is abandoned: not destroyed, its driver not unloaded;
- real handles are swapped as usual;
- if the stuck thread ever returns, it must not mistake its error for a new loss, nor mix handles from the old device with the new one. A global epoch counts device replacements; each thread notes it on entering the layer. A thread whose epoch is out of date knows the device was replaced under it: a device-lost result is then answered by repeating the call on the new device without reporting anything, and the entry points that make several real calls in a row (creating a buffer or image and its memory, binding memory) stop after the first if the epoch has moved.

The cost is a leaked dead device and, for that process, device files that stay open on hardware that is gone. Orderly migration keeps its unbounded wait.

The epoch check covers the entry points audited as making more than one real call with handles from the first. Others re-read their handles at each call and so pick up the new device. A driver that returns success from a device that was replaced meanwhile, in an entry point not covered, could still mix devices; this is recorded as a residual risk.

### D8. Testing

**Fault injection.** `ZSS_TEST_LOSE_AT_SUBMIT=N` makes the layer treat the process's Nth submit as having returned device-lost, without issuing it. The real device is healthy, so teardown is exercised for real. `ZSS_TEST_STUCK_MS=T` holds one extra thread inside the gate for T milliseconds to exercise D7, and `ZSS_LOSS_GRACE_MS` shortens the grace period so the test does not take long.

The layer logs each recovery with the number of submits completed before it. The guest tests read that line to learn at which frame the history was lost, since there the moment depends on when the card left.

**Knowing what the right picture is.** The test application's history image is GPU-generated, so after a loss it is zeroed and frames legitimately differ from an undisturbed run. A new option, `--forget-history-at N`, makes the application itself zero the history before frame N. A recovered run with the loss at frame N must then match a reference run with `--forget-history-at N` byte for byte on the same driver. That one comparison checks the texture (retention), the uniform and vertex buffers (shadows), pipelines, descriptors, command replay, the depth buffer and the re-issued frame.

**Which daemon answer is exercised where.**

| Scenario | Environment | GPU on bus? | Expected |
| :--- | :--- | :--- | :--- |
| Injected loss, managed address is the fake one | Host | No | State `lost`, evacuate to software |
| Injected loss on the NVIDIA GPU, dry-run backend | Host | Yes | Rebuild in place, state stays `attached` |
| Injected loss, no other GPU allowed | Host | No | Park, then resume on attach |
| Injected loss, retention off | Host | No | Texture counted as lost |
| Card pulled over QMP under an application | Guest | No | `lost` within 2 s, evacuate, stale handles listed, return re-attaches |
| Detach after the card was pulled | Guest | No | Refused |

**What stays untested.** A link that dies under a bound real driver. On this laptop the nearest thing is cutting gmux power with the NVIDIA driver active, which belongs to the gated stage of `zss-happy-path` and is not attempted here.

## Risks / Trade-offs

- **A real dead driver may misbehave in ways injection does not show** (hang in destroy, crash on unload, return odd errors instead of device-lost) → D7 covers hangs; destroy and unload of a lost device are best effort; the remaining unknowns are recorded as the reason real-hardware testing is still required.
- **Retained data can be stale relative to the image** if the layer misses a way the GPU can write to it → Any use as a render target or as the destination of a GPU-side operation drops the record. Storage images written by shaders are treated as GPU-dirty from the moment they are bound for writing.
- **Hashing every upload costs load time** → MurmurHash3 runs at several GB/s; `ZSS_RETAIN=off` removes it.
- **The store can fill a disk** → Size cap and trimming; it lives under the cache directory, which users expect to be disposable.
- **Hard links need the pin directory and the store on one file system** → Both are under the same directory by construction.
- **Abandoning a device leaks it** → Only after the grace period, only on loss, and logged.
- **Re-issuing a submit can repeat side effects** if the driver executed part of it before reporting loss → The device it ran on is gone, so its effects are gone with it; only the new device's execution counts.
- **Two detectors can race** → The daemon ignores a `lost` report for a GPU it is already evacuating; the layer ignores an `evacuate` for a device it has already recovered.

## Migration Plan

Additive. A layer without this change ignores `evacuate` and behaves as before; a daemon without it ignores `lost`. The store directory is created on first use and can be deleted at any time while no application under the layer is running.

## Open Questions

- Measured (see `docs/device-loss.md`): hashing runs at about 4.6 GB/s, a first launch pays about a memory copy per upload, a second launch pays the hash only. The 256 MB queue and 4 GiB store defaults stand.
- Whether storage images deserve finer tracking than "dirty once bound for writing". Deferrable: the coarse rule is safe.
