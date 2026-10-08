#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Powering a suspend-in-place GPU off and on under a running desktop.

Runs on the host with the `fake` backend, a device made of files, so the
whole sequence, its refusals, wake requests, the idle timer and recovery can
be exercised without touching hardware. What only hardware can show (the
real driver, the real gmux) is in docs/track-c.md.
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_frames import compare
from zsstest import App, Daemon, FAKE_PCI, ZSSD, check, layered_gpus, reference_frames, run_scenarios

BOUND = {"ZSS_BIND_PCI": FAKE_PCI}
work = tempfile.mkdtemp(prefix="zss-track-off-")
SOFTWARE = next((g for g in layered_gpus() if "llvmpipe" in g), None)
count = 0


class Fake:
    """The files the fake backend reads and writes, and a stand-in for systemctl."""

    def __init__(self, wake=True):
        global count
        count += 1
        self.dir = os.path.join(work, f"fake{count}")
        os.makedirs(self.dir)
        self.write("power", "1")
        if wake:
            self.write("wake", "0")
        self.systemctl = os.path.join(self.dir, "systemctl")
        with open(self.systemctl, "w") as f:
            f.write(f"#!/bin/sh\necho \"$@\" >> '{self.dir}/services'\nexit 0\n")
        os.chmod(self.systemctl, 0o755)

    def write(self, name, text):
        with open(os.path.join(self.dir, name), "w") as f:
            f.write(text)

    def read(self, name):
        path = os.path.join(self.dir, name)
        return open(path).read() if os.path.exists(path) else ""

    def lines(self, name):
        return [l for l in self.read(name).split("\n") if l]

    def daemon(self, *args):
        d = Daemon("--gpu", f"{FAKE_PCI}=fake", "--default-target", "software", "--stop-services", "svc-a",
                   "--service-cmd", self.systemctl, *args, daemon_env=dict(os.environ, ZSSD_FAKE_DIR=self.dir))
        d.args = ["--config", d.conf, "--gpu", f"{FAKE_PCI}=fake", "--runtime-dir", d.dir, "--stop-services", "svc-a",
                  "--service-cmd", self.systemctl]
        return d


def state(d):
    rc, out = d.ctl("status", FAKE_PCI)
    return out.split("state=")[1].split()[0], out


def wait_state(d, want, timeout=6.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if state(d)[0] == want:
            return
        time.sleep(0.05)
    raise_state = state(d)
    check(False, f"state is {raise_state[0]}, expected {want}: {d.log_text()[-600:]}")


def off_and_on_report_every_step():
    f = Fake()
    d = f.daemon()
    try:
        rc, out = d.ctl("off", FAKE_PCI)
        check(rc == 0, "off failed: " + out)
        for want in ("[ZrnSelectiveSuspend] Suspending device: " + FAKE_PCI, "wake on demand: yes", "[1/7] In use by",
                     "[2/7] Applications", "[3/7] Services stopped: svc-a", "[4/7] PCI configuration saved",
                     "[5/7] Driver", "[6/7] Power cut through fake", "[7/7] Watching for wake requests",
                     f"Device {FAKE_PCI} is powered off", f"zssctl on {FAKE_PCI}"):
            check(want in out, f"progress is missing '{want}':\n{out}")
        st, text = state(d)
        check(st == "powered-off" and "wake=yes" in text, text)
        check(f.lines("events") == ["suspend", "power off"], f"driver was not suspended before power was cut: {f.lines('events')}")
        check("stop svc-a" in f.lines("services"), f.read("services"))
        check(os.path.exists(os.path.join(d.dir, FAKE_PCI + ".off")), "no recovery marker was written")

        rc, out = d.ctl("on", FAKE_PCI)
        check(rc == 0, "on failed: " + out)
        for want in ("[ZrnSelectiveSuspend] Resuming device: " + FAKE_PCI, "reason: requested", "[1/4] Power restored",
                     "[2/4]", "[3/4] Services started: svc-a", "[4/4] Applications", "is powered on"):
            check(want in out, f"progress is missing '{want}':\n{out}")
        check(state(d)[0] == "attached", "not attached after on")
        check(f.lines("events")[2:] == ["power on", "resume"], f"power was not restored before the resume: {f.lines('events')}")
        check("start svc-a" in f.lines("services"), f.read("services"))
        check(not os.path.exists(os.path.join(d.dir, FAKE_PCI + ".off")), "the recovery marker was left behind")
        rc, out = d.ctl("on", FAKE_PCI)
        check(rc != 0 and "not powered off" in out, "on of a powered device was not refused: " + out)
    finally:
        d.stop()


def wake_request_powers_on():
    f = Fake()
    d = f.daemon()
    try:
        check(d.ctl("off", FAKE_PCI)[0] == 0, "off failed")
        time.sleep(1.0)
        check(state(d)[0] == "powered-off", "the device did not stay off with no wake request")
        start = time.time()
        f.write("wake", "1")
        wait_state(d, "attached", 3.0)
        check(time.time() - start < 3.0, "waking took longer than three seconds")
        check("a program called into the suspended driver" in d.log_text(), d.log_text()[-400:])
        check("woken=1" in state(d)[1], "the wake was not counted: " + state(d)[1])
        check(f.lines("events")[-2:] == ["power on", "resume"], str(f.lines("events")))
    finally:
        d.stop()


def refusals_change_nothing():
    f = Fake()
    d = f.daemon()
    try:
        f.write("display", "DP-1")
        rc, out = d.ctl("off", FAKE_PCI)
        check(rc != 0 and "driving a display (DP-1)" in out, "a device driving a display was powered off: " + out)
        check(state(d)[0] == "attached" and not f.lines("events"), "a refused power-off touched the device")
        os.remove(os.path.join(f.dir, "display"))

        f.write("fail_suspend", "")
        rc, out = d.ctl("off", FAKE_PCI)
        check(rc != 0 and "refused to suspend" in out and "power was not cut" in out, "wrong result for a failed suspend: " + out)
        check(state(d)[0] == "attached" and f.read("power") == "1", "power was cut although the driver did not suspend")
        check(f.lines("services")[-1] == "start svc-a", "services were not started again: " + f.read("services"))
    finally:
        d.stop()

    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run")
    try:
        rc, out = d.ctl("off", FAKE_PCI)
        check(rc != 0 and "use detach" in out, "off was accepted for a device that is not suspend-in-place: " + out)
    finally:
        d.stop()


def requested_off_stays_off():
    """Switched off on request, the device comes back for the display server and for nobody else."""
    f = Fake()
    d = f.daemon()
    xorg = os.path.join(work, "Xorg")
    shutil.copy(shutil.which("sleep"), xorg)
    other = subprocess.Popen(["sleep", "120"])
    server = subprocess.Popen([xorg, "120"])
    try:
        rc, text = d.ctl("off", FAKE_PCI)
        check(rc == 0 and "stays off (the display server may borrow it for a moment)" in text, text)
        # A monitoring tool asks the driver something: it waits, the device stays off.
        f.write("wake", f"1\nwaiting 1\n{other.pid}\n")
        time.sleep(1.0)
        st, text = state(d)
        check(st == "powered-off" and f.read("power") == "0" and "waiting=1" in text, "the device was woken for a bystander: " + text)
        check(f"sleep (pid {other.pid}) asked for {FAKE_PCI}" in d.log_text(), "the waiting program was not logged")
        # The caller gave up; later the display server calls in: that cannot wait.
        f.write("wake", f"2\nwaiting 0\n")
        time.sleep(0.6)
        check(state(d)[0] == "powered-off", "the device was woken although nobody waits")
        f.write("wake", f"3\nwaiting 2\n{other.pid}\n{server.pid}\n")
        # It is powered for the call and goes off again by itself, staying "off" throughout.
        deadline = time.time() + 3
        while f.read("power") != "1":
            check(time.time() < deadline, "the device was not powered for the display server")
            time.sleep(0.02)
        check(state(d)[0] == "powered-off", "serving the display server changed the state")
        deadline = time.time() + 5
        while f.read("power") != "0":
            check(time.time() < deadline, "the device did not go off again after serving the display server")
            time.sleep(0.05)
        st, text = state(d)
        check(st == "powered-off" and "served=1" in text, text)
        check(f"Xorg needed {FAKE_PCI}" in d.log_text(), "the reason is not in the log")
        # Switching it on while it is being served keeps it on.
        f.write("wake", f"5\nwaiting 1\n{server.pid}\n")
        time.sleep(0.5)
        check(d.ctl("on", FAKE_PCI)[0] == 0 and f.read("power") == "1", "on while serving failed")
        time.sleep(2.5)
        check(state(d)[0] == "attached" and f.read("power") == "1", "the device went off after being switched on")

        # More callers than the driver can list: the display server may be among them.
        check(d.ctl("off", FAKE_PCI)[0] == 0, "second off failed")
        f.write("wake", f"6\nwaiting 40\n{other.pid}\n")
        wait_state(d, "attached", 5.0)
    finally:
        other.kill()
        server.kill()
        other.wait()
        server.wait()
        d.stop()


def idle_off_wakes_for_anyone():
    f = Fake()
    d = f.daemon("--idle-timeout", "0.4")
    other = subprocess.Popen(["sleep", "120"])
    try:
        wait_state(d, "powered-off", 5.0)
        f.write("wake", f"1\nwaiting 1\n{other.pid}\n")
        wait_state(d, "attached", 5.0)
    finally:
        other.kill()
        other.wait()
        d.stop()


def without_wake_support_it_stays_off():
    f = Fake(wake=False)
    d = f.daemon()
    try:
        check("wake=no" in state(d)[1], state(d)[1])
        rc, out = d.ctl("off", FAKE_PCI)
        check(rc == 0 and "wake on demand: no" in out and "No wake support" in out, out)
        time.sleep(0.7)
        check(state(d)[0] == "powered-off", "not off")
        check(d.ctl("on", FAKE_PCI)[0] == 0 and state(d)[0] == "attached", "on failed")
    finally:
        d.stop()


def applications_are_moved_first():
    if not SOFTWARE:
        return "no software renderer installed"
    ref, out = os.path.join(work, "app-ref"), os.path.join(work, "app-out")
    os.makedirs(out)
    reference_frames("llvmpipe", 48, ref)
    f = Fake()
    d = f.daemon()
    app = App(d, ["--gpu", f"[ZSS {FAKE_PCI}]", "--frames", 48, "--delay-ms", 40, "--out", out], BOUND)
    try:
        app.wait_frame(8)
        rc, text = d.ctl("off", FAKE_PCI)
        check(rc == 0 and "zss-testapp (application, will be moved)" in text and "1 moved to software" in text,
              "the application was not moved first: " + text)
        check("moving applications: 0 of 1" in text and "moving applications: 1 of 1 (zss-testapp, moved)" in text,
              "each application moved is not reported as it happens: " + text)
        check("migrated-away" in state(d)[1], state(d)[1])
        seen = app.last_frame()
        app.wait_frame(seen + 5)

        # A wake brings the device back but leaves the application where it is.
        f.write("wake", "1")
        wait_state(d, "attached")
        check("migrated-away" in state(d)[1], "a wake request pulled the application back: " + state(d)[1])
        check("left where they are" in d.log_text(), d.log_text()[-300:])

        check(d.ctl("off", FAKE_PCI)[0] == 0, "second off failed")
        rc, text = d.ctl("on", FAKE_PCI, "--stay")
        check(rc == 0 and "left where they are" in text, "on --stay brought the application back: " + text)
        check("migrated-away" in state(d)[1], state(d)[1])

        check(d.ctl("off", FAKE_PCI)[0] == 0, "third off failed")
        rc, text = d.ctl("on", FAKE_PCI)
        check(rc == 0 and "returned to the device" in text, "on did not bring the application back: " + text)
        check("moving applications: 1 of 1 (zss-testapp, moved)" in text,
              "the application brought back is not reported as it happens: " + text)
        check("migrated-away" not in state(d)[1], state(d)[1])
        rc, err = app.finish()
        check(rc == 0, f"application exited with {rc}: {err[-300:]}")
    finally:
        app.kill()
        d.stop()
    ok, message = compare(ref, out)
    check(ok, message)


def frozen(pid):
    """Whether the process sits in a ZSS freezer cgroup that reports itself frozen."""
    try:
        cgroup = open(f"/proc/{pid}/cgroup").read().strip().split("::", 1)[1]
        return f"zss-parked-{pid}" in cgroup and "frozen 1" in open(f"/sys/fs/cgroup{cgroup}/cgroup.events").read()
    except OSError:
        return False


def outsiders_are_frozen_not_refused():
    """A process the layer cannot move waits, frozen, while the device is off, and carries on afterwards."""
    f = Fake()
    d = f.daemon()
    outsider = subprocess.Popen(["sleep", "120"])
    f.write("holders", str(outsider.pid))
    try:
        rc, text = d.ctl("off", FAKE_PCI)
        if rc != 0:
            # Without the right to freeze it, the process must stop the power-off rather than be ignored.
            check("could not be frozen" in text and state(d)[0] == "attached" and f.read("power") == "1", text)
            return "this user may not freeze processes here; the refusal was checked instead (the QEMU test freezes for real)"
        check("sleep (not under ZSS_AirLock, will be frozen)" in text and f"processes frozen: sleep (pid {outsider.pid})" in text, text)
        check(frozen(outsider.pid), "the process is not frozen while the device is off")
        rc, text = d.ctl("on", FAKE_PCI)
        check(rc == 0 and "processes thawed: 1" in text, text)
        check(not frozen(outsider.pid) and outsider.poll() is None, "the process was not thawed, or did not survive")

        # The terminal the command is typed in is never frozen: nobody could ask for the device back.
        f.write("holders", f"{outsider.pid} {os.getpid()}")
        rc, text = d.ctl("off", FAKE_PCI)
        check(rc != 0 and "this command was run from it" in text and not frozen(outsider.pid) and f.read("power") == "1",
              "the requester's own parent was not spared: " + text)
        f.write("holders", str(outsider.pid))

        # A plain detach still refuses: the device might not come back the same.
        rc, text = d.ctl("detach", FAKE_PCI)
        check(rc != 0 and "ZSSDetachBlocked" in text and "not started under ZSS_AirLock" in text, "detach did not refuse: " + text)
    finally:
        outsider.kill()
        outsider.wait()
        d.stop()


def idle_timer_never_freezes():
    f = Fake()
    d = f.daemon("--idle-timeout", "0.4")
    outsider = subprocess.Popen(["sleep", "120"])
    f.write("holders", str(outsider.pid))
    try:
        time.sleep(2.0)
        check(state(d)[0] == "attached" and not f.lines("events") and not frozen(outsider.pid),
              "the idle timer powered off a device that a process was using")
        os.remove(os.path.join(f.dir, "holders"))
        wait_state(d, "powered-off", 5.0)
    finally:
        outsider.kill()
        outsider.wait()
        d.stop()


def unmovable_layer_application_waits_frozen():
    if not SOFTWARE:
        return "no software renderer installed"
    out = os.path.join(work, "block-out")
    os.makedirs(out)
    f = Fake()
    d = f.daemon()
    app = App(d, ["--gpu", f"[ZSS {FAKE_PCI}]", "--frames", 40, "--delay-ms", 40, "--out", out, "--use-untracked"], BOUND)
    try:
        app.wait_frame(5)
        rc, text = d.ctl("off", FAKE_PCI)
        if rc != 0:
            check("could not be frozen" in text and state(d)[0] == "attached" and f.read("power") == "1", text)
            check(app.finish()[0] == 0, "the application did not keep running after the refusal")
            return "this user may not freeze processes here; the refusal was checked instead"
        check("zss-testapp (cannot be moved, will be frozen)" in text, text)
        before = len(os.listdir(out))
        time.sleep(0.8)
        check(len(os.listdir(out)) == before and frozen(app.pid), "the application kept running while the device was off")
        check(d.ctl("on", FAKE_PCI)[0] == 0, "on failed")
        rc, err = app.finish()
        check(rc == 0 and len(os.listdir(out)) == 40, f"the application did not carry on after the thaw: {err[-200:]}")
    finally:
        app.kill()
        d.stop()


def idle_timer_and_damping():
    f = Fake()
    d = f.daemon("--idle-timeout", "0.6", "--quick-wake", "30")
    try:
        wait_state(d, "powered-off", 5.0)
        check("idle timer" in d.log_text(), "the automatic power-off was not labelled: " + d.log_text()[-400:])
        # Woken three times in a row shortly after an automatic power-off: the wait doubles.
        for n in range(1, 4):
            f.write("wake", str(n))
            wait_state(d, "attached", 3.0)
            if n < 3:
                wait_state(d, "powered-off", 5.0)
        check("keeps being woken; next automatic power-off after 1 s" in d.log_text(),
              "the wait was not lengthened:\n" + d.log_text()[-700:])
        check("woken=3" in state(d)[1] and "idle-off=1s" in state(d)[1], state(d)[1])
        t = time.time()
        wait_state(d, "powered-off", 6.0)
        check(time.time() - t > 0.9, "the lengthened wait was not observed")
    finally:
        d.stop()

    f = Fake()
    d = f.daemon()
    try:
        time.sleep(2.0)
        check(state(d)[0] == "attached" and not f.lines("events"), "the device was powered off with no timeout configured")
    finally:
        d.stop()


def idle_timer_spares_a_device_in_use():
    if not SOFTWARE:
        return "no software renderer installed"
    out = os.path.join(work, "inuse-out")
    os.makedirs(out)
    f = Fake()
    d = f.daemon("--idle-timeout", "0.5")
    app = App(d, ["--gpu", f"[ZSS {FAKE_PCI}]", "--frames", 60, "--delay-ms", 50, "--out", out], BOUND)
    try:
        app.wait_frame(5)
        time.sleep(2.0)
        check(state(d)[0] == "attached" and not f.lines("events"), "a device in use was powered off by the idle timer")
        check(app.finish()[0] == 0, "the application failed")
        wait_state(d, "powered-off", 5.0)  # and once it is free, the timer does act
    finally:
        app.kill()
        d.stop()


def nothing_is_left_off_when_the_daemon_goes():
    # Killed: the next start, or `zssd --recover`, puts the device back.
    f = Fake()
    d = f.daemon()
    try:
        check(d.ctl("off", FAKE_PCI)[0] == 0, "off failed")
        d.proc.send_signal(signal.SIGKILL)
        d.proc.wait()
        check(f.read("power") == "0", "the device is not off after the daemon was killed")
        r = subprocess.run([ZSSD, *d.args, "--recover"], env=dict(os.environ, ZSSD_FAKE_DIR=f.dir),
                           capture_output=True, text=True, timeout=20)
        check(r.returncode == 0 and "was left powered off; restoring it" in r.stderr, "recover did nothing: " + r.stderr)
        check(f.read("power") == "1" and f.lines("events")[-2:] == ["power on", "resume"], "the device was not restored")
        check("start svc-a" in f.lines("services"), "services were not started again")
        check(not os.path.exists(os.path.join(d.dir, FAKE_PCI + ".off")), "the marker was not removed")
        r = subprocess.run([ZSSD, *d.args, "--recover"], env=dict(os.environ, ZSSD_FAKE_DIR=f.dir),
                           capture_output=True, text=True, timeout=20)
        check("nothing to recover" in r.stderr, "a second recover found something: " + r.stderr)
    finally:
        d.stop()

    # Stopped cleanly: it powers the device on itself before exiting.
    f = Fake()
    d = f.daemon()
    try:
        check(d.ctl("off", FAKE_PCI)[0] == 0, "off failed")
        d.proc.send_signal(signal.SIGTERM)
        d.proc.wait(10)
        check(f.read("power") == "1" and f.lines("events")[-2:] == ["power on", "resume"],
              "a stopping daemon left the device off")
        check("the daemon is stopping" in d.log_text(), d.log_text()[-300:])
    finally:
        d.stop()


def settings_come_from_the_file():
    f = Fake()
    conf = os.path.join(f.dir, "zssd.conf")
    sock_dir = tempfile.mkdtemp(prefix="zss-", dir=os.environ.get("XDG_RUNTIME_DIR") or "/tmp")
    with open(conf, "w") as c:
        c.write(f"# test configuration\ngpu = {FAKE_PCI}=fake\nsocket = {sock_dir}/s\nruntime_dir = {sock_dir}\n"
                f"idle_timeout = 0.5   # seconds\nstop_services = svc-a\nservice_cmd = {f.systemctl}\n")
    env = dict(os.environ, ZSSD_FAKE_DIR=f.dir)
    proc = subprocess.Popen([ZSSD, "--config", conf], env=env, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.time() + 6
        while f.read("power") != "0":
            check(time.time() < deadline and proc.poll() is None, "the timeout from the file had no effect")
            time.sleep(0.05)
    finally:
        proc.terminate()
        err = proc.communicate(timeout=10)[1]
        shutil.rmtree(sock_dir, ignore_errors=True)
    check("idle devices are powered off after 0 s" in err or "idle devices are powered off after 1 s" in err, err[-300:])

    with open(conf, "w") as c:
        c.write("no_such_setting = 1\n")
    r = subprocess.run([ZSSD, "--config", conf], env=env, capture_output=True, text=True, timeout=10)
    check(r.returncode != 0 and "unknown or invalid setting 'no_such_setting'" in r.stderr, "a bad file was accepted: " + r.stderr)


if __name__ == "__main__":
    try:
        rc = run_scenarios([
            ("off and on report every step, in the right order", off_and_on_report_every_step),
            ("a wake request powers the device on", wake_request_powers_on),
            ("refusals change nothing", refusals_change_nothing),
            ("switched off on request, only the display server brings it back", requested_off_stays_off),
            ("switched off by the idle timer, anyone brings it back", idle_off_wakes_for_anyone),
            ("without wake support the device stays off until switched on", without_wake_support_it_stays_off),
            ("applications are moved first, stay put on a wake, return on request", applications_are_moved_first),
            ("processes outside the layer are frozen, not refused; detach still refuses", outsiders_are_frozen_not_refused),
            ("a layer application that cannot be moved waits frozen", unmovable_layer_application_waits_frozen),
            ("the idle timer never freezes a process", idle_timer_never_freezes),
            ("idle timer powers off, and backs off when woken repeatedly", idle_timer_and_damping),
            ("idle timer spares a device in use", idle_timer_spares_a_device_in_use),
            ("nothing is left off when the daemon is killed or stopped", nothing_is_left_off_when_the_daemon_goes),
            ("settings come from the configuration file", settings_come_from_the_file),
        ])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(rc)
