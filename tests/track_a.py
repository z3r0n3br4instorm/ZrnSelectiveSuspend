#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Track A: graphics migration on the host, with nothing powered off.

Each scenario runs zss-testapp under the layer, asks zssd (dry-run backend)
to detach and re-attach the GPU while frames are being produced, and compares
every frame with a run that used no layer and was never migrated.

Scenarios that need hardware this machine lacks are skipped, not failed.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_frames import compare
from zsstest import (App, Daemon, FAKE_PCI, Failure, check, layered_gpus, reference_frames,
                     run_scenarios, ZSS_RUN)

FRAMES = 48
BOUND = {"ZSS_BIND_PCI": FAKE_PCI}
work = tempfile.mkdtemp(prefix="zss-track-a-")
gpus = layered_gpus()


def find(name):
    return next((g for g in gpus if name in g), None)


NVIDIA, INTEL, SOFTWARE = find("NVIDIA"), find("Intel"), find("llvmpipe")
PCI = {}


def pci_of(name):
    """PCI address of a real GPU, from sysfs by vendor."""
    vendor = {"NVIDIA": "0x10de", "Intel": "0x8086"}[name]
    if name not in PCI:
        base = "/sys/bus/pci/devices"
        for dev in sorted(os.listdir(base)):
            try:
                cls = open(f"{base}/{dev}/class").read().strip()
                ven = open(f"{base}/{dev}/vendor").read().strip()
            except OSError:
                continue
            if cls.startswith("0x03") and ven == vendor:
                PCI[name] = dev
                break
    return PCI.get(name)


def migrate_and_compare(tag, source_gpu, source_pci, target, reference_gpu, tolerance, app_env=None,
                        released=None, extra_args=()):
    """Detach around frame 10, attach around frame 26, and compare all frames."""
    ref = os.path.join(work, tag + "-ref")
    out = os.path.join(work, tag + "-out")
    os.makedirs(out)
    reference_frames(reference_gpu, FRAMES, ref)

    d = Daemon("--gpu", f"{source_pci}=dry-run", "--allow-software")
    app = App(d, ["--gpu", source_gpu, "--frames", FRAMES, "--delay-ms", 40, "--out", out, *extra_args], app_env)
    try:
        app.wait_frame(10)
        rc, text = d.ctl("detach", source_pci, "--to", target)
        check(rc == 0, "detach failed: " + text)
        rc, status = d.ctl("status")
        check("migrated-away" in status, "application is not reported as migrated away: " + status)
        if released:
            held = app.open_device_files(released)
            check(not held, f"application still holds {held} after migrating away")
        mid = app.last_frame()
        app.wait_frame(max(mid + 8, 26))
        rc, text = d.ctl("attach", source_pci)
        check(rc == 0, "attach failed: " + text)
        rc, status = d.ctl("status")
        check("migrated-away" not in status and "parked" not in status,
              "application did not return to its origin GPU: " + status)
        rc, err = app.finish()
        check(rc == 0, f"application exited with {rc}: {err[-400:]}")
        check(app.pid and "frame %d" % (FRAMES - 1) in app.lines, "application did not render every frame")
    finally:
        app.kill()
        d.stop()
    ok, message = compare(ref, out, tolerance, 0.002 if tolerance else 0.0)
    check(ok, message)


def same_driver():
    if not SOFTWARE:
        return "no software renderer installed"
    migrate_and_compare("same", f"[ZSS {FAKE_PCI}]", FAKE_PCI, "software", "llvmpipe", 0, BOUND)


def same_driver_secondary():
    if not SOFTWARE:
        return "no software renderer installed"
    migrate_and_compare("same-sec", f"[ZSS {FAKE_PCI}]", FAKE_PCI, "software", "llvmpipe", 0, BOUND,
                        extra_args=["--secondary"])


def nvidia_to_intel_secondary():
    if not (NVIDIA and INTEL):
        return "needs an NVIDIA and an Intel GPU"
    migrate_and_compare("nv-intel-sec", "NVIDIA", pci_of("NVIDIA"), pci_of("Intel"), "NVIDIA", 2,
                        released="/dev/nvidia", extra_args=["--secondary"])


def nvidia_to_intel():
    if not (NVIDIA and INTEL):
        return "needs an NVIDIA and an Intel GPU"
    migrate_and_compare("nv-intel", "NVIDIA", pci_of("NVIDIA"), pci_of("Intel"), "NVIDIA", 2,
                        released="/dev/nvidia")


def nvidia_to_software():
    if not (NVIDIA and SOFTWARE):
        return "needs an NVIDIA GPU and a software renderer"
    migrate_and_compare("nv-sw", "NVIDIA", pci_of("NVIDIA"), "software", "NVIDIA", 2, released="/dev/nvidia")


def weaker_target_parks():
    """An application using a feature the target lacks is parked, then resumes intact.

    Under the native profile, where each GPU offers all of its own features."""
    if not (SOFTWARE and INTEL):
        return "needs a software renderer and an Intel GPU as the weaker target"
    ref = os.path.join(work, "park-ref")
    out = os.path.join(work, "park-out")
    os.makedirs(out)
    reference_frames("llvmpipe", FRAMES, ref)

    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run")
    app = App(d, ["--gpu", f"[ZSS {FAKE_PCI}]", "--frames", FRAMES, "--delay-ms", 40, "--out", out,
                  "--enable-all-features"], {**BOUND, "ZSS_PROFILE": "native"})
    try:
        app.wait_frame(10)
        rc, text = d.ctl("detach", FAKE_PCI, "--to", pci_of("Intel"))
        check(rc == 0, "detach failed: " + text)
        rc, status = d.ctl("status")
        check("parked" in status, "application was not parked: " + status)

        # A parked application renders nothing.
        before = len(os.listdir(out))
        time.sleep(1.0)
        check(len(os.listdir(out)) == before, "a parked application kept rendering")

        # Asking it to resume while its GPU is away must fail with the named error.
        rc, text = d.ctl("resume", app.pid)
        check(rc != 0 and "ZSSFailedResumeNoDRM" in text, "resume did not fail as specified: " + text)
        rc, status = d.ctl("status")
        check("parked" in status, "application left the parked state after a failed resume: " + status)

        rc, text = d.ctl("attach", FAKE_PCI)
        check(rc == 0, "attach failed: " + text)
        rc, err = app.finish()
        check(rc == 0, f"application exited with {rc}: {err[-400:]}")
    finally:
        app.kill()
        d.stop()
    ok, message = compare(ref, out)
    check(ok, message)


def portable_profile_reaches_weaker_target():
    """The same application, enabling everything it is offered, moves to the weaker GPU and back.

    Under the portable profile what it is offered is what both GPUs have, so nothing holds it."""
    if not (SOFTWARE and INTEL):
        return "needs a software renderer and an Intel GPU as the weaker target"
    migrate_and_compare("profile", f"[ZSS {FAKE_PCI}]", FAKE_PCI, pci_of("Intel"), "llvmpipe", 2, BOUND,
                        extra_args=["--enable-all-features"])


def started_while_away_moves_there_on_return():
    """A program that asked for the NVIDIA GPU while it was away starts elsewhere, and moves there when it returns."""
    if not (NVIDIA and INTEL):
        return "needs an NVIDIA and an Intel GPU"
    nv = pci_of("NVIDIA")
    ref = os.path.join(work, "away-ref")
    out = os.path.join(work, "away-out")
    os.makedirs(out)
    reference_frames("Intel", FRAMES, ref)

    d = Daemon("--gpu", f"{nv}=dry-run")
    app = None
    try:
        rc, text = d.ctl("detach", nv)
        check(rc == 0, "dry detach failed: " + text)
        app = App(d, ["--gpu", "Intel", "--frames", FRAMES, "--delay-ms", 40, "--out", out], {"ZSS_START_ON": nv})
        app.wait_frame(8)
        check(not app.open_device_files("/dev/nvidia"), "the program opened the NVIDIA GPU while it was away")
        rc, text = d.ctl("attach", nv)
        check(rc == 0, "attach failed: " + text)
        check(app.open_device_files("/dev/nvidia"), "the program was not moved to the NVIDIA GPU when it returned")
        rc, err = app.finish()
        check(rc == 0, f"application exited with {rc}: {err[-400:]}")
        check("frame %d" % (FRAMES - 1) in app.lines, "application did not render every frame")
    finally:
        if app:
            app.kill()
        d.stop()
    ok, message = compare(ref, out, 2, 0.002)
    check(ok, message)


def bystander_is_left_alone():
    """An application that never used the GPU is not moved by its detach or attach."""
    if not SOFTWARE:
        return "no software renderer installed"
    ref = os.path.join(work, "bystander-ref")
    out_user = os.path.join(work, "bystander-user")
    out_other = os.path.join(work, "bystander-other")
    os.makedirs(out_user)
    os.makedirs(out_other)
    reference_frames("llvmpipe", FRAMES, ref)

    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run", "--allow-software")
    user = App(d, ["--gpu", f"[ZSS {FAKE_PCI}]", "--frames", FRAMES, "--delay-ms", 40, "--out", out_user], BOUND)
    # Started on the plain software renderer, without the binding that makes the
    # renderer pose as the managed GPU: it has nothing to do with that GPU.
    other = App(d, ["--gpu", "llvmpipe", "--frames", FRAMES, "--delay-ms", 40, "--out", out_other])
    try:
        user.wait_frame(8)
        other.wait_frame(4)
        # Staying on one driver keeps the frames byte-exact.
        for step, extra in (("detach", ["--to", "software"]), ("attach", [])):
            rc, text = d.ctl(step, FAKE_PCI, *extra)
            check(rc == 0, f"{step} failed: " + text)
            rc, status = d.ctl("status")
            check(f"pid {other.pid}" not in status, f"the bystander was drawn into the {step}: " + status)
            time.sleep(0.4)
        for app in (user, other):
            rc, err = app.finish()
            check(rc == 0, f"application exited with {rc}: {err[-300:]}")
    finally:
        user.kill()
        other.kill()
        d.stop()
    for out in (out_user, out_other):
        ok, message = compare(ref, out)
        check(ok, message)


def untracked_feature_is_named():
    if not SOFTWARE:
        return "no software renderer installed"
    out = os.path.join(work, "untracked-out")
    os.makedirs(out)
    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run", "--allow-software")
    app = App(d, ["--gpu", f"[ZSS {FAKE_PCI}]", "--frames", 30, "--delay-ms", 40, "--out", out,
                  "--use-untracked"], BOUND)
    try:
        app.wait_frame(5)
        rc, status = d.ctl("status")
        check("non-migratable" in status and "query pools" in status,
              "the untracked feature is not named in the status: " + status)
        rc, text = d.ctl("detach", FAKE_PCI)
        check(rc != 0 and "ZSSDetachBlocked" in text and "query pools" in text,
              "detach was not blocked with the feature named: " + text)
        rc, status = d.ctl("status")
        check("migrated-away" not in status and "state=attached" in status, status)
        rc, err = app.finish()
        check(rc == 0, f"a non-migratable application must keep running; it exited with {rc}: {err[-300:]}")
    finally:
        app.kill()
        d.stop()


def without_daemon():
    """With no daemon the layer must simply render."""
    if not SOFTWARE:
        return "no software renderer installed"
    ref = os.path.join(work, "nodaemon-ref")
    out = os.path.join(work, "nodaemon-out")
    os.makedirs(out)
    reference_frames("llvmpipe", 12, ref)
    app = App(None, ["--gpu", "llvmpipe", "--frames", 12, "--out", out], {"ZSS_SOCKET": "/nonexistent"})
    rc, err = app.finish()
    check(rc == 0, f"application exited with {rc}: {err[-300:]}")
    ok, message = compare(ref, out)
    check(ok, message)


def vkcube_cycle():
    """A real windowed application survives a migration and the trip back."""
    vkcube = shutil.which("vkcube")
    if not vkcube or not os.environ.get("DISPLAY"):
        return "needs vkcube and an X display"
    if not SOFTWARE:
        return "no software renderer installed"
    names = layered_gpus(BOUND)
    index = next(i for i, n in enumerate(names) if FAKE_PCI in n)
    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run", "--allow-software")
    # Implicit layers reorder devices; keep vkcube's numbering equal to the layer's.
    env = dict(BOUND, VK_LOADER_LAYERS_DISABLE="~implicit~")
    app = App(d, ["--gpu_number", index, "--c", 500], env, program=vkcube)
    try:
        time.sleep(2.5)
        rc, text = d.ctl("detach", FAKE_PCI, "--to", "software")
        check(rc == 0, "detach failed: " + text)
        rc, status = d.ctl("status")
        check("vkcube" in status and "migrated-away" in status, "vkcube did not migrate: " + status)
        time.sleep(1.5)
        check(app.proc.poll() is None, "vkcube died after migrating")
        rc, text = d.ctl("attach", FAKE_PCI)
        check(rc == 0, "attach failed: " + text)
        time.sleep(1.0)
        check(app.proc.poll() is None, "vkcube died after migrating back")
        rc, err = app.finish(60)
        check(rc == 0, f"vkcube exited with {rc}: {err[-300:]}")
    finally:
        app.kill()
        d.stop()


def opengl_cycle():
    """An OpenGL program, drawn through Zink, moves NVIDIA to Intel and back and keeps drawing.

    The NVIDIA 470 driver has neither dynamic rendering nor maintenance5, which
    Zink requires: on it the layer stands in for both.
    """
    gears = shutil.which("glxgears")
    if not gears or not os.environ.get("DISPLAY"):
        return "needs glxgears and an X display"
    if not (NVIDIA and INTEL):
        return "needs an NVIDIA and an Intel GPU"
    nv, intel = pci_of("NVIDIA"), pci_of("Intel")
    d = Daemon("--gpu", f"{nv}=dry-run")
    app = App(d, [], {"ZSS_START_ON": "dedicated"}, program=gears, run_args=["--gl"])
    try:
        # glxgears reports its frame rate every five seconds, and only if frames were drawn.
        app.wait_containing("frames in", 30)
        rc, status = d.ctl("status")
        check("glxgears" in status and "non-migratable" not in status.split("glxgears")[1].splitlines()[0],
              "glxgears is not migratable: " + status)
        rc, text = d.ctl("detach", nv, "--to", intel)
        check(rc == 0, "detach failed: " + text)
        rc, status = d.ctl("status")
        check("migrated-away" in status, "glxgears did not move to Intel: " + status)
        app.wait_containing("frames in", 30)
        rc, text = d.ctl("attach", nv)
        check(rc == 0, "attach failed: " + text)
        rc, status = d.ctl("status")
        check("migrated-away" not in status and "parked" not in status, "glxgears did not move back: " + status)
        app.wait_containing("frames in", 30)
        check(app.proc.poll() is None, "glxgears died")
    finally:
        app.kill()
        d.stop()


def opengl_swap_interval():
    """An OpenGL program that keeps changing its swap interval keeps drawing, on each GPU.

    Each change makes Zink replace the window's swapchain and, from another
    thread, still present a frame to the old one. The NVIDIA 470 driver hangs
    on that frame unless the layer keeps it back.
    """
    glapp = os.path.join(os.environ["ZSS_BUILD"], "tests", "zss-glapp")
    if not os.path.exists(glapp) or not os.environ.get("DISPLAY"):
        return "needs the OpenGL test program and an X display"
    ran = 0
    for name, have in (("NVIDIA", NVIDIA), ("Intel", INTEL)):
        if not have:
            continue
        for run in range(3):
            app = App(None, ["--frames", 240, "--flip-every", 7], {"ZSS_SOCKET": "/nonexistent", "ZSS_START_ON": pci_of(name)},
                      program=glapp, run_args=["--gl"])
            try:
                app.wait_containing("frame 240", 40)
            finally:
                app.kill()
        ran += 1
    if not ran:
        return "needs an NVIDIA or an Intel GPU"


if __name__ == "__main__":
    try:
        rc = run_scenarios([
            ("layer renders identically with no daemon", without_daemon),
            ("same-driver migration and return, exact frames", same_driver),
            ("NVIDIA to Intel and back", nvidia_to_intel),
            ("secondary command buffers: same-driver migration and return, exact frames", same_driver_secondary),
            ("secondary command buffers: NVIDIA to Intel and back", nvidia_to_intel_secondary),
            ("NVIDIA to software and back", nvidia_to_software),
            ("weaker target parks, failed resume is named, origin resumes intact", weaker_target_parks),
            ("portable profile: everything offered is enabled, and the weaker target is still reachable",
             portable_profile_reaches_weaker_target),
            ("started while its GPU was away, moved there when it returns", started_while_away_moves_there_on_return),
            ("an application on another GPU is left alone", bystander_is_left_alone),
            ("untracked feature is reported and blocks detach", untracked_feature_is_named),
            ("vkcube survives migration and return", vkcube_cycle),
            ("OpenGL through Zink: NVIDIA to Intel and back, still drawing", opengl_cycle),
            ("OpenGL through Zink: changing the swap interval over and over", opengl_swap_interval),
        ])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(rc)
