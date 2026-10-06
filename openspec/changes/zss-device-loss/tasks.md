## 1. Daemon: noticing a loss and telling the truth

- [x] 1.1 Add the `lost` state to the state machine, `zssctl status` and `zssctl monitor`, with an event on entering it
- [x] 1.2 Cache each managed GPU's device numbers at start-up and after every attach, and make the holder scan use the cache
- [x] 1.3 List processes holding a GPU's nodes after it has left the bus with the class `stale`
- [x] 1.4 Add a kernel uevent socket to the poll loop, keep a one-second presence poll as fallback, and move an attached GPU that is absent to `lost`
- [x] 1.5 Make sure a GPU leaving the bus during a detach the daemon is running never becomes `lost`
- [x] 1.6 Refuse a detach of a lost GPU with an error saying the device is already gone
- [x] 1.7 Treat a lost GPU that reappears as an attach, and make an explicit attach fail while it is still absent
- [x] 1.8 Extend `tests/track_b_guest.py`: pull the card with no detach and check `lost` within two seconds, the event, the refused detach, and re-attach on return

## 2. Protocol

- [x] 2.1 Document `lost` (layer to daemon) and `evacuate` (daemon to layer), the `stale` client class, the `lost` state and the lost-contents count in `docs/protocol.md`
- [x] 2.2 Daemon: on becoming `lost`, send `evacuate` to every registered client of that GPU with a target chosen as for a detach, and collect outcomes
- [x] 2.3 Daemon: on a client's `lost` report, check the bus; if the GPU is present send `evacuate` naming the same GPU and keep the state, otherwise go to `lost`
- [x] 2.4 Daemon: ignore a `lost` report for a GPU already being evacuated (implemented; no test produces two simultaneous reports)
- [x] 2.5 Freeze clients that report parked after an evacuation, as after a detach

## 3. Layer: recovery without readback

- [x] 3.1 Add `ZSS_TEST_LOSE_AT_SUBMIT` fault injection
- [x] 3.2 Add a recovery wait: an entry point that sees device-lost marks the device, reports `lost`, leaves the gate, and blocks until recovery finishes
- [x] 3.3 Route device-lost from every real call that can return it through the recovery wait, then repeat the real call with the new handles
- [x] 3.4 Implement the rebuild from held state: shadows for mapped buffers, explicit zero-fill for everything else with contents, and a count of zeroed objects
- [x] 3.5 Track pending fences and create them signalled when rebuilding after a loss
- [x] 3.6 Re-issue the submit that was lost and return its result
- [x] 3.7 Handle `evacuate`: recover onto the named target, park when it is unsuitable or absent, rebuild in place when it is the current GPU
- [x] 3.8 Fall back to a target of the layer's own choosing when there is no daemon or no `evacuate` arrives within five seconds, and return device-lost when nothing is suitable (the no-daemon case is tested; the five-second wait on a silent daemon is not)
- [x] 3.9 Make teardown of the lost device best effort, and still unload its driver library
- [x] 3.10 Give non-migratable devices the device-lost error unchanged
- [x] 3.11 Send the outcome with the lost-contents count, and log it

## 4. Layer: a stuck thread

- [x] 4.1 Give the gate a timed hold for loss recovery; keep the unbounded hold for migration
- [x] 4.2 On timeout, abandon the old device instead of destroying it, and log that it was abandoned
- [x] 4.3 Audit entry points that make several real calls, and stop them after the first if the device was replaced under the thread (changed from reading handles into locals; see design D7)
- [x] 4.4 Add `ZSS_TEST_STUCK_MS` and test that other threads continue on the target while one is held

## 5. Test application and host scenarios for recovery

- [x] 5.1 Add `--forget-history-at N` to `zss-testapp`
- [x] 5.2 Test injected loss with the GPU absent from the bus: state `lost`, application continues on software, and every frame matches the `--forget-history-at N` reference
- [x] 5.3 Test injected loss on a present GPU: rebuilt in place, state stays `attached`
- [x] 5.4 Test injected loss with nowhere to go: parked, then resumed on attach
- [x] 5.5 Test that the lost GPU's return moves the application back to it
- [x] 5.6 Test that an application using an untracked feature receives device-lost
- [x] 5.7 Test the no-daemon fallback: recovery onto a GPU the layer picks

## 6. Retention store

- [x] 6.1 Implement MurmurHash3 x64-128 with test vectors
- [x] 6.2 Implement the store: content-addressed files under the cache directory, write-then-rename, lookup, read
- [x] 6.3 Implement the bounded write queue and writer thread, with the unretained fallback when the queue is full
- [x] 6.4 Record retained uploads at submit time for buffer-to-image copies, buffer-to-buffer copies into unshadowed buffers, and `UpdateBuffer`, including one-hop lineage
- [x] 6.5 Drop records and mark the destination GPU-dirty on render-target use, blit, resolve, clear, unreadable copies, and storage-image binding; drop records on destroy
- [x] 6.6 Pin depended-on blobs with hard links in `pin-<pid>/`, and remove the pin directory at exit
- [x] 6.7 Implement trimming: least-recently-used blobs with link count one, and pin directories of dead processes
- [x] 6.8 Implement `ZSS_RETAIN=disk|ram|off`
- [x] 6.9 Restore retained uploads during a rebuild after loss
- [x] 6.10 Tighten 5.2: with retention on, every frame from N onward matches the reference byte for byte
- [x] 6.11 Test a second launch writes nothing new, identical uploads are stored once, and `ZSS_RETAIN=off` yields a texture counted as lost
- [x] 6.12 Test trimming removes unpinned data and leaves pinned data readable
- [x] 6.13 Test a full queue: submits complete and the affected texture is counted as lost

## 7. Guest scenarios with an application

- [x] 7.1 Pull the card under a running application: `lost` within two seconds, the application is evacuated and keeps rendering, it no longer holds the card's node
- [x] 7.2 Check a process outside the layer is listed as `stale` after the pull
- [x] 7.3 Return the card: automatic attach, the application is bound to it again
- [x] 7.4 Pull the card with no other target allowed: the application parks and is frozen, and resumes when the card returns

## 8. Measurements and records

- [x] 8.1 Measure upload-time overhead with retention on, first and second launch, and time to first frame after recovery; record the numbers in `docs/`
- [x] 8.2 Settle the default store limit and queue bound from those numbers
- [x] 8.3 Record in `docs/` what remains untested (a link dying under a bound real driver) and what a hardware test of it would need
- [x] 8.4 Add `ZSS_RETAIN` and the new test variables to the README table
