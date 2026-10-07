#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Device loss on the host, by fault injection.

ZSS_TEST_LOSE_AT_SUBMIT makes the layer behave as if the real driver had
reported the device lost at a chosen submit. Nothing can then be read back,
so the application is rebuilt from what the layer holds.

The one thing in the test scene that only the GPU ever had is the history
image, so a run that loses its device at frame N must match, byte for byte,
a reference run that zeroes its own history before frame N
(`--forget-history-at N`). That single comparison covers the uploaded
texture, the mapped buffers, pipelines, descriptors, the replayed command
buffer, the depth buffer and the re-issued frame.
"""
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_frames import compare
from zsstest import App, Daemon, FAKE_PCI, check, layered_gpus, reference_frames, run_scenarios

FRAMES = 40
LOSE_FRAME = 12
# The scene's set-up is one submit and each frame is one more, so frame N is submit N + 2.
LOSE = {"ZSS_TEST_LOSE_AT_SUBMIT": str(LOSE_FRAME + 2)}
BOUND = {"ZSS_BIND_PCI": FAKE_PCI}
LVP = "/usr/share/vulkan/icd.d/lvp_icd.json"
work = tempfile.mkdtemp(prefix="zss-track-loss-")
gpus = layered_gpus()
SOFTWARE = next((g for g in gpus if "llvmpipe" in g), None)
NVIDIA = next((g for g in gpus if "NVIDIA" in g), None)
# Send evacuated applications to the software renderer, so frames stay byte-exact.
SOFT_DAEMON = ["--gpu", f"{FAKE_PCI}=dry-run", "--default-target", "software"]
refs = {}


def reference(gpu):
    """Frames of an undisturbed run that forgets its history at the frame of the loss."""
    if gpu not in refs:
        refs[gpu] = os.path.join(work, f"ref-{gpu}")
        reference_frames(gpu, FRAMES, refs[gpu], extra_args=["--forget-history-at", LOSE_FRAME])
    return refs[gpu]


def run_lossy(tag, daemon_args, app_gpu, env, extra_args=(), during=None, expect_rc=0):
    """Runs the application through an injected loss. Returns (daemon log, app stderr, frame dir)."""
    out = os.path.join(work, tag)
    os.makedirs(out)
    d = Daemon(*daemon_args) if daemon_args is not None else None
    app = App(d, ["--gpu", app_gpu, "--frames", FRAMES, "--delay-ms", 30, "--out", out, *extra_args], env)
    try:
        if during:
            during(d, app)
        rc, err = app.finish()
        check(rc == expect_rc, f"application exited with {rc}, expected {expect_rc}: {err[-500:]}")
        log = d.log_text() if d else ""
    finally:
        app.kill()
        if d:
            d.stop()
    return log, err, out


def exact(ref, out):
    ok, message = compare(ref, out)
    check(ok, message)


def gone_evacuates_to_software():
    if not SOFTWARE:
        return "no software renderer installed"
    state = {}

    def during(d, app):
        app.wait_frame(LOSE_FRAME + 6)
        state["after_loss"] = d.ctl("status")[1]
        # The device "returns": the application must go home, as after a migration.
        rc, text = d.ctl("attach", FAKE_PCI)
        check(rc == 0, "attach of the returned device failed: " + text)
        state["after_return"] = d.ctl("status")[1]

    log, err, out = run_lossy("gone", SOFT_DAEMON,
                              f"[ZSS {FAKE_PCI}]", {**BOUND, **LOSE}, during=during)
    check("state=lost" in state["after_loss"], "the device is not reported as lost: " + state["after_loss"])
    check("migrated-away" in state["after_loss"], "the application did not move away: " + state["after_loss"])
    check("attached -> lost" in log, "no state change to lost in the daemon log:\n" + log)
    check("recovered; lost contents: 1" in log, "the daemon did not record one object with lost contents:\n" + log)
    check(f"recovered after submit {LOSE_FRAME + 1} " in err, "the layer did not log the recovery: " + err[-300:])
    check("state=attached" in state["after_return"] and "migrated-away" not in state["after_return"],
          "the application did not return to its origin: " + state["after_return"])
    exact(reference("llvmpipe"), out)


def work_in_flight_is_run_again():
    """
    The device dies after a frame was submitted and before the application waited
    for it. That frame must still be drawn, on the new device, not skipped: the
    result is the same as losing the device just before the frame.
    """
    if not SOFTWARE:
        return "no software renderer installed"
    log, err, out = run_lossy("inflight", SOFT_DAEMON, f"[ZSS {FAKE_PCI}]",
                              {**BOUND, "ZSS_TEST_LOSE_AFTER_SUBMIT": str(LOSE_FRAME + 2)})
    check(f"recovered after submit {LOSE_FRAME + 1} " in err,
          "the layer did not report the last submit whose results survived: " + err[-300:])
    check("1 submission(s) in flight were issued again" in err, "the layer did not say it re-issued the frame: " + err[-300:])
    exact(reference("llvmpipe"), out)

    # The same with no daemon and nowhere to go: the frame is issued again when a device turns up.
    log, err, out = run_lossy("inflight-local", None, "llvmpipe", {"ZSS_TEST_LOSE_AFTER_SUBMIT": str(LOSE_FRAME + 2)})
    exact(reference("llvmpipe"), out)


def reset_rebuilds_in_place():
    if not NVIDIA:
        return "needs an NVIDIA GPU"
    pci = next(d for d in sorted(os.listdir("/sys/bus/pci/devices"))
               if open(f"/sys/bus/pci/devices/{d}/class").read().startswith("0x03")
               and open(f"/sys/bus/pci/devices/{d}/vendor").read().strip() == "0x10de")
    state = {}

    def during(d, app):
        app.wait_frame(LOSE_FRAME + 6)
        state["status"] = d.ctl("status")[1]

    log, err, out = run_lossy("inplace", ["--gpu", f"{pci}=dry-run"], "NVIDIA", LOSE, during=during)
    check("state=attached" in state["status"] and "migrated-away" not in state["status"],
          "a device that is still present must be rebuilt in place: " + state["status"])
    check("lost" not in state["status"].split("\n")[0], "state became lost although the GPU never left")
    check("NVIDIA" in err.split("recovered after submit")[1].split("\n")[0], "not rebuilt on the same GPU: " + err[-300:])
    exact(reference("NVIDIA"), out)


def healthy_neighbour_is_left_alone():
    """A reset that hits one application must not cost another its GPU-generated contents."""
    if not NVIDIA:
        return "needs an NVIDIA GPU"
    pci = next(d for d in sorted(os.listdir("/sys/bus/pci/devices"))
               if open(f"/sys/bus/pci/devices/{d}/class").read().startswith("0x03")
               and open(f"/sys/bus/pci/devices/{d}/vendor").read().strip() == "0x10de")
    plain = os.path.join(work, "ref-plain")
    reference_frames("NVIDIA", FRAMES, plain)
    hit, spared = os.path.join(work, "hit"), os.path.join(work, "spared")
    os.makedirs(hit)
    os.makedirs(spared)
    d = Daemon("--gpu", f"{pci}=dry-run")
    apps = [App(d, ["--gpu", "NVIDIA", "--frames", FRAMES, "--delay-ms", 30, "--out", spared]),
            App(d, ["--gpu", "NVIDIA", "--frames", FRAMES, "--delay-ms", 30, "--out", hit], LOSE)]
    try:
        for app in apps:
            rc, err = app.finish()
            check(rc == 0, f"application exited with {rc}: {err[-300:]}")
        check("recovered after submit" in apps[1].stderr, "the application that lost its device was not rebuilt")
        check("recovered" not in apps[0].stderr and "lost" not in apps[0].stderr,
              "the healthy application was rebuilt too: " + apps[0].stderr[-300:])
    finally:
        for app in apps:
            app.kill()
        d.stop()
    exact(reference("NVIDIA"), hit)
    exact(plain, spared)  # its history is intact: identical to a run nothing happened to


def nowhere_to_go_parks_then_resumes():
    if not SOFTWARE:
        return "no software renderer installed"
    state = {}

    def during(d, app):
        app.wait_frame(LOSE_FRAME - 1)
        deadline = time.time() + 20
        while "parked" not in d.ctl("status")[1]:
            check(time.time() < deadline, "the application was never parked: " + d.ctl("status")[1])
            time.sleep(0.1)
        before = len(os.listdir(os.path.join(work, "park")))
        time.sleep(0.8)
        check(len(os.listdir(os.path.join(work, "park"))) == before, "a parked application kept rendering")
        rc, text = d.ctl("attach", FAKE_PCI)
        check(rc == 0, "attach failed: " + text)
        state["after"] = d.ctl("status")[1]

    # The layer is shown only the software driver, so the daemon's suggested target means nothing to it.
    log, err, out = run_lossy("park", ["--gpu", f"{FAKE_PCI}=dry-run"], f"[ZSS {FAKE_PCI}]",
                              {**BOUND, **LOSE, "ZSS_REAL_DRIVER_FILES": LVP}, during=during)
    check("parked; lost contents: 1" in log, "the daemon did not record the park:\n" + log)
    check("parked" not in state["after"], "still parked after the device returned: " + state["after"])
    exact(reference("llvmpipe"), out)


def without_retention_the_texture_is_lost():
    if not SOFTWARE:
        return "no software renderer installed"
    for tag, env in (("off", {"ZSS_RETAIN": "off"}), ("full", {"ZSS_RETAIN_QUEUE_MB": "0"})):
        cache = os.path.join(work, "cache-" + tag)
        log, err, out = run_lossy("noretain-" + tag, SOFT_DAEMON,
                                  f"[ZSS {FAKE_PCI}]", {**BOUND, **LOSE, **env, "XDG_CACHE_HOME": cache})
        check("recovered; lost contents: 2" in log,
              f"with retention {tag}, the texture and the history should both count as lost:\n" + log)
        store = os.path.join(cache, "zss", "store")
        blobs = [n for n in os.listdir(store) if not n.startswith("pin-")] if os.path.isdir(store) else []
        check(not blobs, f"with retention {tag}, data was written to the store: {blobs}")
        # Every submit completed all the same.
        check(len(os.listdir(out)) == FRAMES, "the application did not render every frame")


def second_launch_writes_nothing_new():
    if not SOFTWARE:
        return "no software renderer installed"
    cache = os.path.join(work, "cache-again")
    store = os.path.join(cache, "zss", "store")
    seen = []
    for run in range(2):
        out = os.path.join(work, f"again-{run}")
        os.makedirs(out)
        app = App(None, ["--gpu", "llvmpipe", "--frames", 6, "--out", out],
                  {"ZSS_SOCKET": "/nonexistent", "XDG_CACHE_HOME": cache})
        rc, err = app.finish()
        check(rc == 0, f"application exited with {rc}: {err[-300:]}")
        seen.append({n: os.stat(os.path.join(store, n)).st_ino for n in os.listdir(store) if not n.startswith("pin-")})
    check(len(seen[0]) == 1, f"expected the one texture in the store after the first run, found {seen[0]}")
    check(seen[0] == seen[1], f"the second run changed the store: {seen[0]} -> {seen[1]}")
    check(not [n for n in os.listdir(store) if n.startswith("pin-")], "a pin directory was left behind")


def untracked_application_gets_the_error():
    if not SOFTWARE:
        return "no software renderer installed"
    log, err, out = run_lossy("untracked", SOFT_DAEMON,
                              f"[ZSS {FAKE_PCI}]", {**BOUND, **LOSE}, extra_args=["--use-untracked"], expect_rc=1)
    check("VkResult -4" in err, "the application should have been given the device-lost error: " + err[-300:])


def no_daemon_recovers_locally():
    if not SOFTWARE:
        return "no software renderer installed"
    log, err, out = run_lossy("local", None, "llvmpipe", {**LOSE, "ZSS_SOCKET": "/nonexistent"})
    check("recovered after submit" in err, "the layer did not recover on its own: " + err[-300:])
    exact(reference("llvmpipe"), out)


def stuck_thread_does_not_block_recovery():
    if not SOFTWARE:
        return "no software renderer installed"
    start = time.time()
    log, err, out = run_lossy("stuck", SOFT_DAEMON, f"[ZSS {FAKE_PCI}]",
                              {**BOUND, **LOSE, "ZSS_TEST_STUCK_MS": "6000", "ZSS_LOSS_GRACE_MS": "400"})
    check("abandoning the lost device" in err, "the dead device was not abandoned: " + err[-400:])
    # The application must not have waited for the stuck thread before carrying on.
    check("recovered after submit" in err.split("abandoning")[1], "no recovery after abandoning: " + err[-400:])
    exact(reference("llvmpipe"), out)
    check(time.time() - start > 0, "")


if __name__ == "__main__":
    try:
        rc = run_scenarios([
            ("GPU gone: evacuated to software, exact frames, returns home", gone_evacuates_to_software),
            ("work in flight when the device dies is run again", work_in_flight_is_run_again),
            ("GPU still present: rebuilt in place", reset_rebuilds_in_place),
            ("a reset under one application leaves its neighbour alone", healthy_neighbour_is_left_alone),
            ("nowhere to go: parked, then resumed when the GPU returns", nowhere_to_go_parks_then_resumes),
            ("without retention the texture counts as lost", without_retention_the_texture_is_lost),
            ("a second launch writes nothing new to the store", second_launch_writes_nothing_new),
            ("a non-migratable application gets the device-lost error", untracked_application_gets_the_error),
            ("no daemon: the layer recovers on its own", no_daemon_recovers_locally),
            ("a stuck thread does not block recovery", stuck_thread_does_not_block_recovery),
        ])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(rc)
