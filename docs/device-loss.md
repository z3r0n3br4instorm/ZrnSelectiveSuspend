# When a GPU is lost

What the system does when a GPU goes away without a detach, what it costs,
and what has not been tested. Numbers were measured on the reference laptop
(i7-3720QM, btrfs on SSD, Linux 7.2.2) in October 2026.

## What survives

After a loss nothing can be read from the device. An application under the
layer is rebuilt from what the layer holds:

| State | After a loss |
| :--- | :--- |
| Objects, pipelines, descriptor sets, command buffers | Rebuilt from their stored creation parameters and recordings |
| Buffers written through mapped memory | Restored from the layer's shadow of that memory |
| Images and device-local buffers filled by a copy from such a buffer | Restored from the retention store |
| The submit during which the loss was reported | Issued again on the new device |
| Submits accepted earlier and not yet seen to finish | Issued again on the new device, in order (see "Work that was in flight") |
| Fences the application was waiting on for that work | Pending again, and signalled when the re-issued work finishes |
| Fences for lost work that cannot be repeated | Signalled, so that nobody waits for ever |
| Images the GPU rendered and later frames build on | **Zero-filled, counted and reported** |
| Images cleared and redrawn every frame | Zero-filled, not counted: the next frame replaces them |

An application marked non-migratable is not recovered; it receives
`VK_ERROR_DEVICE_LOST` as it would without the layer.

What is retained is deliberately narrow: an upload is kept only when it
replaces a whole image subresource (or a whole buffer) from tightly packed
data. Partial updates are not retained, and their destinations count as lost.

## Work that was in flight

A submission the device had accepted, and that the application had not yet
seen finish, dies with the device. The layer keeps a short list of such
submissions (dropped as soon as a fence, or a wait for the queue or device to
go idle, shows them done) and issues them again, in order, on the device the
application is rebuilt on. The application then waits for its fence as if
nothing had happened, and reads back what the frame should have produced.

They run against the rebuilt state, so anything only the lost device held is
blank for them as for everything after: losing the device with a frame in
flight gives the same result as losing it just before that frame. The layer's
log line says so: "recovered after submit N" names the last submission whose
results survived, and "N submission(s) in flight were issued again" follows
when there were any.

Limits: semaphores are not replayed (the re-issued work is ordered by the
queue alone), at most sixteen submissions are remembered, and one whose
command buffer has been reset or freed since cannot be repeated and is
dropped.

Before this was done the frame in flight was skipped and the application read
back the previous frame's image for it. It showed as a rare one-frame mismatch
in the pulled-card tests, when the card happened to leave in the few
microseconds between a submit and the wait for it.

## A thread stuck in the dead driver

A driver can be waiting on something outside itself when its device dies. The
proprietary NVIDIA driver waits, with no timeout, for the X server to
acknowledge a present; when the card has lost power that acknowledgement never
comes, and the thread never leaves the driver. Rebuilding the application on
another GPU is then no use to it.

So the layer never gives a driver the application's own connection to the X
server. Each driver gets one the layer opened, and when the daemon reports the
driver's device gone the layer shuts those connections first. The wait fails,
the driver returns an error, and the layer treats any error from a device it
knows to be lost as that loss: the thread joins the recovery and repeats its
call on the new device. A thread that still does not come out within the
grace period (three seconds, `ZSS_LOSS_GRACE_MS`) is left behind as before,
and whatever it brings back later, even success, is discarded and the call
repeated.

This came out of the first real power cut on the reference laptop
(`docs/track-c.md`). It has been tested in QEMU, where no X server is
involved, and not yet against that failure itself.

## Cost of retention

`build/tests/bench_retain`, 4 MB blobs, default settings:

| Case | On the submitting thread | Background |
| :--- | :--- | :--- |
| Hashing alone | 4.5–4.9 GB/s | |
| First launch, 256 MB of new data | 0.14 s (1.8 GB/s): hash, then copy into the write queue | 0.10 s more until on disk |
| Second launch, same 256 MB | 0.055 s (4.6 GB/s): hash and find it already stored | nothing written |
| Burst of 1 GB of new data at memory speed | 0.60 s | 167 of 256 blobs retained; the rest hit the 256 MB queue bound and went ahead unretained |

So a first launch pays roughly a memory copy per upload, and later launches
pay a hash. The last row is the bound doing its job: an application that
produces new data faster than the disk takes it is never stalled, and what
could not be queued is reported as lost if the GPU is lost later. A real
application uploads at the speed of the GPU, far below that burst.

The defaults stay at a 256 MB queue and a 4 GiB store.

## Time to recover

Rebuild of the test scene (four images, three buffers, two pipelines, one
recorded command buffer) in place after an injected loss:

| GPU | Rebuild |
| :--- | :--- |
| Intel HD 4000 | 12 ms |
| llvmpipe | 21 ms |
| NVIDIA GT 650M | 72 ms |

This is dominated by device creation and pipeline compilation, so a large
application will take proportionally longer. When the loss is first seen by
the application rather than the daemon, add the time the daemon needs to
answer. With no daemon the layer decides at once; with a daemon that never
answers it waits five seconds first.

A thread stuck inside the dead driver delays recovery by the grace period
(three seconds by default), after which the dead device is abandoned instead
of destroyed.

## How it is tested

| What | Where |
| :--- | :--- |
| Loss with the GPU gone, in place, with nowhere to go, without retention, without a daemon, with a stuck thread, and for a non-migratable application | `tests/track_loss.py`, on the host, by fault injection |
| The store: reuse across runs, deduplication, trimming, pins, a full queue, `off` and `ram` | `tests/test_retain.c` |
| A card pulled with no detach: `lost` within two seconds, refused detach, stale handles, evacuation, parking and freezing, return | `tests/track_b_guest.py`, in QEMU as root |

The host tests check recovery by exact frame comparison. The test scene's
history image is the one thing only the GPU ever had, so a run that loses its
GPU at frame N must match a run that zeroes its own history before frame N
(`zss-testapp --forget-history-at N`).

## What is not tested

**A link that dies under a bound real driver.** Every loss so far is either
injected (the real device is healthy and is torn down normally) or, in QEMU,
a removal the guest kernel was warned about five seconds ahead. QEMU ignores
an attempt to drop the link from inside the guest, so it cannot produce the
real thing. What a real dead driver may do that injection does not show:

- block in a call instead of returning an error (the stuck-thread path
  covers one thread; several, or the thread doing the teardown, are untested);
- crash or hang when its objects are destroyed or its library is unloaded;
- return errors other than `VK_ERROR_DEVICE_LOST`;
- fault on access to memory it had mapped. The layer stops touching a
  device's mappings once it knows the device is lost, but a write made just
  before that is not protected.

**What a hardware test needs.** One of:

- an eGPU on Thunderbolt or OCuLink, pulled while an application under the
  layer renders on it; or
- on the reference laptop, cutting gmux power with the NVIDIA driver active.
  This needs root and can hang the machine, and belongs with the gated stage
  of `zss-happy-path`.

In both cases the things to record are the kernel log, whether the
application's calls return or block, and whether tearing down the old driver
succeeds.

**Also untested:** a daemon that is connected but never answers a loss report
(the five-second fallback); two applications reporting the same loss at the
same moment; Wayland surfaces.
