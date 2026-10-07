#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Track B, guest side. Runs as root inside the QEMU guest started by track_b.py.

Exercises what cannot be done safely on the host: releasing a display card
from the kernel, cutting slot power, having the card physically leave and
return, and freezing a parked application. The card is QEMU's `bochs-display`;
the guest has no accelerated GPU, so the test application renders on the
software renderer while posing as the card (ZSS_BIND_PCI), which makes it a
real client holding the card's device node.
"""
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_frames import compare
from zsstest import App, Daemon, Failure, ZSSCTL, ZSSD, check, reference_frames, run_scenarios

WORK = "/mnt"
FRAMES = 56
LVP = "/usr/share/vulkan/icd.d/lvp_icd.json"
requests = 0
CARD = None


def host(action):
    """Asks the host to do something to the virtual hardware and waits for the answer."""
    global requests
    requests += 1
    # Written under another name first, so the host never reads a half-written request.
    with open(f"{WORK}/pending-{requests:03d}", "w") as f:
        f.write(action)
    os.rename(f"{WORK}/pending-{requests:03d}", f"{WORK}/req-{requests:03d}")
    ack = f"{WORK}/ack-{requests:03d}"
    deadline = time.time() + 60
    while not os.path.exists(ack):
        check(time.time() < deadline, f"the host did not answer '{action}'")
        time.sleep(0.05)
    time.sleep(0.05)
    with open(ack) as f:
        return f.read().strip()


def find_card():
    for dev in glob.glob("/sys/bus/pci/devices/*"):
        try:
            if open(dev + "/vendor").read().strip() == "0x1234":
                return os.path.basename(dev)
        except OSError:
            pass
    return None


def driver_of(pci):
    link = f"/sys/bus/pci/devices/{pci}/driver"
    return os.path.basename(os.path.realpath(link)) if os.path.exists(link) else None


def card_nodes():
    return sorted(glob.glob("/dev/dri/card*"))


def slot_dir():
    for s in glob.glob("/sys/bus/pci/slots/*"):
        if open(s + "/address").read().strip() == CARD.rsplit(".", 1)[0]:
            return s
    raise Failure("the card's slot was not found")


def wait_for(predicate, what, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise Failure(f"timed out waiting for {what}")


def ensure_card():
    """Puts the card back and waits for its driver, so each scenario starts the same way."""
    if not find_card():
        host("plug")
    wait_for(lambda: find_card() and driver_of(CARD) and card_nodes(), "the card and its driver")


def state_of(d):
    rc, out = d.ctl("status", CARD)
    check(rc == 0, out)
    return out.split("state=")[1].split()[0], out


def daemon(*args):
    ensure_card()
    return Daemon("--gpu", CARD, *args, runtime_dir="/run")


def app_env():
    return {"ZSS_BIND_PCI": CARD, "ZSS_REAL_DRIVER_FILES": LVP}


def start_app(d, out, *extra):
    os.makedirs(out, exist_ok=True)
    return App(d, ["--gpu", f"[ZSS {CARD}]", "--frames", FRAMES, "--delay-ms", 50, "--out", out, *extra], app_env())


def reference(tag, *extra):
    ref = f"/tmp/{tag}-ref"
    reference_frames("llvmpipe", FRAMES, ref, {"VK_DRIVER_FILES": LVP}, extra_args=extra)
    return ref


def pull_card():
    """Removes the card with no detach. Returns the time at which it left the bus."""
    check(host("unplug") == "ok", "the host could not pull the card")
    wait_for(lambda: find_card() is None, "the card to leave the bus")
    return time.time()


def forgotten_at(stderr):
    """
    The frame before which the application's GPU-generated history was lost, read
    from the layer's own log line. A submit is one frame, after one set-up submit.
    """
    for line in stderr.split("\n"):
        if " after submit " in line and ("recovered" in line or "parked" in line):
            return int(line.split(" after submit ")[1].split()[0]) - 1
    raise Failure("the layer logged no recovery: " + stderr[-400:])


def what_the_layer_said(stderr, daemon_log):
    """For a frame mismatch: every migration and recovery the layer and the daemon recorded."""
    lines = [l for l in stderr.split("\n") if any(w in l for w in ("submit", "recover", "migrat", "park", "lost", "resum"))]
    return ("\n--- layer:\n" + "\n".join(lines[-30:]) + "\n--- daemon:\n" + daemon_log[-2500:])


# ---- scenarios ------------------------------------------------------------------


def idle_detach_then_explicit_attach():
    d = daemon("--no-auto-attach")
    try:
        state, text = state_of(d)
        check("backend=pciehp-slot" in text and "removal=supported" in text,
              "the hot-plug slot backend was not detected: " + text)

        rc, text = d.ctl("detach", CARD)
        check(rc == 0, "detach failed: " + text)
        state, _ = state_of(d)
        check(state == "safe-to-remove", f"state is {state}, expected safe-to-remove")
        check(find_card() is None, "the card is still on the bus")
        check(not card_nodes(), "the card's device node still exists")
        check(open(slot_dir() + "/power").read().strip() == "0", "the slot still reports power")

        # With the slot empty there is nothing to attach.
        check(host("unplug") in ("gone", "ok"), "the host could not remove the card")
        rc, text = d.ctl("attach", CARD)
        check(rc != 0, "attach succeeded with no card in the slot")
        state, _ = state_of(d)
        check(state == "safe-to-remove", f"a failed attach left the state at {state}")

        check(host("plug") == "ok", "the host could not return the card")
        wait_for(find_card, "the card to reappear")
        state, _ = state_of(d)
        check(state == "safe-to-remove", f"auto-attach is off, yet the state became {state}")
        rc, text = d.ctl("attach", CARD)
        check(rc == 0, "attach failed: " + text)
        state, _ = state_of(d)
        check(state == "attached", f"state is {state} after attach")
        check(driver_of(CARD), "no driver is bound after attach")
    finally:
        d.stop()


def returning_card_is_attached_automatically():
    d = daemon()
    monitor = subprocess.Popen([ZSSCTL, "monitor"], env=d.env(), stdout=subprocess.PIPE, text=True)
    try:
        time.sleep(0.3)
        rc, text = d.ctl("detach", CARD)
        check(rc == 0, "detach failed: " + text)
        check(state_of(d)[0] == "safe-to-remove", "not safe-to-remove after detach")
        check(host("plug") == "ok", "the host could not return the card")
        wait_for(lambda: state_of(d)[0] == "attached", "the daemon to attach the returning card", 30)
        check(driver_of(CARD), "no driver is bound after the automatic attach")
    finally:
        monitor.terminate()
        events = monitor.communicate()[0]
        d.stop()
    seen = [line.split(" ", 1)[1] for line in events.strip().split("\n") if line]
    want = ["attached -> detaching", "detaching -> safe-to-remove", "safe-to-remove -> attaching",
            "attaching -> attached"]
    check(seen == want, f"state events were {seen}, expected {want}")


def application_follows_the_card():
    ref = reference("follow")
    out = "/tmp/follow-out"
    d = daemon("--allow-software")
    app = start_app(d, out)
    try:
        app.wait_frame(8)
        check(app.open_device_files("/dev/dri/card"), "the application does not hold the card")
        _, text = state_of(d)
        check(f"pid {app.pid}" in text and "migratable" in text, "the application is not listed: " + text)

        rc, text = d.ctl("detach", CARD)
        check(rc == 0, "detach failed: " + text)
        state, text = state_of(d)
        check(state == "safe-to-remove", f"state is {state}, expected safe-to-remove")
        check("migrated-away" in text, "the application is not reported as migrated away: " + text)
        check(not app.open_device_files("/dev/dri/card"), "the application still holds the card")
        check(find_card() is None, "the card is still on the bus")
        seen = app.last_frame()
        app.wait_frame(seen + 6)

        check(host("plug") == "ok", "the host could not return the card")
        wait_for(lambda: state_of(d)[0] == "attached", "the automatic attach", 30)
        _, text = state_of(d)
        check("migrated-away" not in text, "the application did not return to the card: " + text)
        check(app.open_device_files("/dev/dri/card"), "the application does not hold the card after returning")
        rc, err = app.finish()
        check(rc == 0, f"the application exited with {rc}: {err[-400:]}")
    finally:
        app.kill()
        d.stop()
    ok, message = compare(ref, out)
    check(ok, message)


def process_outside_the_layer_blocks():
    d = daemon()
    node = card_nodes()[0]
    holder = subprocess.Popen([sys.executable, "-c", f"import time; f = open('{node}'); time.sleep(60)"])
    try:
        has_it = lambda: any(os.path.realpath(f) == node for f in glob.glob(f"/proc/{holder.pid}/fd/*"))
        wait_for(has_it, "the holder to open the card", 10)
        rc, text = d.ctl("detach", CARD)
        check(rc != 0 and "ZSSDetachBlocked" in text, "detach was not blocked: " + text)
        check(f"pid {holder.pid}" in text and "not started under the ZSS layer" in text,
              "the blocking process is not named: " + text)
        check(state_of(d)[0] == "attached", "a blocked detach changed the state")
        check(find_card() and driver_of(CARD), "a blocked detach touched the device")
    finally:
        holder.kill()
        holder.wait()
        d.stop()


def failed_power_off_is_never_safe_to_remove():
    d = daemon("--no-auto-attach")
    power = slot_dir() + "/power"
    with open("/tmp/stuck-power", "w") as f:
        f.write("1\n")
    # Make the slot's power switch refuse writes and keep reading "on".
    subprocess.run(["mount", "--bind", "/tmp/stuck-power", power], check=True)
    subprocess.run(["mount", "-o", "remount,bind,ro", power], check=True)
    try:
        rc, text = d.ctl("detach", CARD)
        check(rc != 0, "detach reported success although power could not be cut: " + text)
        check("power" in text, "the backend's error was not reported: " + text)
        state, _ = state_of(d)
        check(state not in ("safe-to-remove", "powered-off"), f"state is {state} although power is still on")
    finally:
        subprocess.run(["umount", power])
    try:
        # Power never dropped, so an attach finds the card again.
        rc, text = d.ctl("attach", CARD)
        check(rc == 0, "attach after the failed power-off did not recover: " + text)
        check(state_of(d)[0] == "attached" and driver_of(CARD), "the card did not come back")
    finally:
        d.stop()


def application_with_nowhere_to_go_is_parked_and_frozen():
    ref = reference("park")
    out = "/tmp/park-out"
    d = daemon()  # no other GPU and no software fallback allowed
    app = start_app(d, out)
    try:
        app.wait_frame(8)
        rc, text = d.ctl("detach", CARD)
        check(rc == 0, "detach failed: " + text)
        state, text = state_of(d)
        check(state == "safe-to-remove", f"state is {state}, expected safe-to-remove")
        check("parked" in text, "the application was not parked: " + text)
        check(not app.open_device_files("/dev/dri/card"), "the parked application still holds the card")

        cgroup = open(f"/proc/{app.pid}/cgroup").read().strip().split("::", 1)[1]
        check(f"zss-parked-{app.pid}" in cgroup, f"the parked application is not in a freezer cgroup: {cgroup}")
        check(open(f"/sys/fs/cgroup{cgroup}/cgroup.freeze").read().strip() == "1", "the cgroup is not frozen")
        check("frozen 1" in open(f"/sys/fs/cgroup{cgroup}/cgroup.events").read(), "the process is not frozen")
        before = len(os.listdir(out))
        time.sleep(1.0)
        check(len(os.listdir(out)) == before, "the parked application kept rendering")

        rc, text = d.ctl("resume", app.pid)
        check(rc != 0 and "ZSSFailedResumeNoDRM" in text, "resume with no GPU did not fail as specified: " + text)
        check("parked" in state_of(d)[1], "the application left the parked state after a failed resume")

        check(host("plug") == "ok", "the host could not return the card")
        wait_for(lambda: state_of(d)[0] == "attached", "the automatic attach", 30)
        check("parked" not in state_of(d)[1], "the application is still parked after the card returned")
        check(f"zss-parked-{app.pid}" not in open(f"/proc/{app.pid}/cgroup").read(),
              "the application was not moved out of the freezer cgroup")
        rc, err = app.finish()
        check(rc == 0, f"the application exited with {rc}: {err[-400:]}")
    finally:
        app.kill()
        d.stop()
    ok, message = compare(ref, out)
    check(ok, message)


# ---- the card leaves without being asked ---------------------------------------------


def pulled_card_is_reported_lost():
    d = daemon("--no-auto-attach")
    monitor = subprocess.Popen([ZSSCTL, "monitor"], env=d.env(), stdout=subprocess.PIPE, text=True)
    try:
        time.sleep(0.3)
        gone_at = pull_card()
        wait_for(lambda: state_of(d)[0] == "lost", "the state to become lost", 2.0)
        check(time.time() - gone_at <= 2.5, "the loss was not noticed within two seconds")

        rc, text = d.ctl("detach", CARD)
        check(rc != 0 and "already gone" in text, "detach of a lost device was not refused: " + text)
        check("safe-to-remove" not in text, "a lost device was reported as safe to remove: " + text)
        check(state_of(d)[0] == "lost", "a refused detach changed the state")
        rc, text = d.ctl("attach", CARD)
        check(rc != 0 and "still absent" in text, "attach succeeded with the card absent: " + text)
        check(state_of(d)[0] == "lost", "a failed attach changed the state")

        check(host("plug") == "ok", "the host could not return the card")
        wait_for(find_card, "the card to reappear")
        rc, text = d.ctl("attach", CARD)
        check(rc == 0, "attach of the returned card failed: " + text)
        check(state_of(d)[0] == "attached" and driver_of(CARD), "the card is not attached after returning")
    finally:
        monitor.terminate()
        events = monitor.communicate()[0]
        d.stop()
    seen = [line.split(" ", 1)[1] for line in events.strip().split("\n") if line]
    want = ["attached -> lost", "lost -> attaching", "attaching -> attached"]
    check(seen == want, f"state events were {seen}, expected {want}")


def pulled_card_evacuates_its_application():
    out = "/tmp/pull-out"
    d = daemon("--default-target", "software")
    node = card_nodes()[0]
    outsider = subprocess.Popen([sys.executable, "-c", f"import time; f = open('{node}'); time.sleep(120)"])
    app = start_app(d, out, "--delay-ms", 150)
    try:
        app.wait_frame(6)
        check(app.open_device_files("/dev/dri/card"), "the application does not hold the card")
        pull_card()
        wait_for(lambda: state_of(d)[0] == "lost", "the state to become lost", 2.0)
        wait_for(lambda: "migrated-away" in state_of(d)[1], "the application to be evacuated", 10)
        _, text = state_of(d)
        check(not app.open_device_files("/dev/dri/card"), "the evacuated application still holds the card's node")
        check(f"pid {outsider.pid}" in text and "stale" in text,
              "the process outside the layer is not listed as holding a stale handle: " + text)
        seen = app.last_frame()
        app.wait_frame(seen + 3)

        check(host("plug") == "ok", "the host could not return the card")
        wait_for(lambda: state_of(d)[0] == "attached", "the automatic attach", 30)
        _, text = state_of(d)
        check("migrated-away" not in text, "the application did not return to the card: " + text)
        check(app.open_device_files("/dev/dri/card"), "the application does not hold the card after returning")
        rc, err = app.finish(180)
        check(rc == 0, f"the application exited with {rc}: {err[-400:]}")
        check("recovered; lost contents: 1" in d.log_text(), "the daemon did not record the recovery:\n" + d.log_text())
    finally:
        log = d.log_text()
        outsider.kill()
        outsider.wait()
        app.kill()
        d.stop()
    ok, message = compare(reference("pull", "--forget-history-at", forgotten_at(err)), out)
    check(ok, message + what_the_layer_said(err, log))


def pulled_card_with_nowhere_to_go_parks():
    out = "/tmp/pullpark-out"
    d = daemon()
    app = start_app(d, out, "--delay-ms", 150)
    try:
        app.wait_frame(6)
        pull_card()
        wait_for(lambda: "parked" in state_of(d)[1], "the application to be parked", 10)
        check(state_of(d)[0] == "lost", "the device is not reported as lost")
        cgroup = open(f"/proc/{app.pid}/cgroup").read().strip().split("::", 1)[1]
        check(f"zss-parked-{app.pid}" in cgroup, f"the parked application is not in a freezer cgroup: {cgroup}")
        before = len(os.listdir(out))
        time.sleep(1.0)
        check(len(os.listdir(out)) == before, "the parked application kept rendering")

        check(host("plug") == "ok", "the host could not return the card")
        wait_for(lambda: state_of(d)[0] == "attached", "the automatic attach", 30)
        check("parked" not in state_of(d)[1], "the application is still parked after the card returned")
        rc, err = app.finish(180)
        check(rc == 0, f"the application exited with {rc}: {err[-400:]}")
    finally:
        log = d.log_text()
        app.kill()
        d.stop()
    ok, message = compare(reference("pullpark", "--forget-history-at", forgotten_at(err)), out)
    check(ok, message + what_the_layer_said(err, log))


# ---- progress on the text console -----------------------------------------------------


def progress_is_shown_on_the_console():
    """`zssctl off --console` puts the screen on a text console and prints each step there."""
    if not os.path.exists("/dev/tty0"):
        return "the guest has no virtual consoles"
    fake = "/tmp/fake-console"
    os.makedirs(fake, exist_ok=True)
    for name, text in (("power", "1"), ("wake", "0")):
        with open(f"{fake}/{name}", "w") as f:
            f.write(text)
    active = lambda: open("/sys/class/tty/tty0/active").read().strip()
    screen = lambda: open("/dev/vcs63", "rb").read().decode("latin-1")
    before = active()
    d = Daemon("--gpu", "ffff:00:00.0=fake", "--stop-services", "", runtime_dir="/run",
               daemon_env=dict(os.environ, ZSSD_FAKE_DIR=fake))
    try:
        rc, text = d.ctl("off", "ffff:00:00.0", "--console")
        check(rc == 0, "off --console failed: " + text)
        check(active() == "tty63", f"the screen is on {active()}, not on the ZSS console")
        shown = screen()
        for want in ("[ZrnSelectiveSuspend] Suspending device: ffff:00:00.0", "[5/7] Driver", "[6/7] Power cut",
                     "is powered off", "Press any key to power it on"):
            check(want in shown, f"the console does not show '{want}'")
        rc, text = d.ctl("on", "ffff:00:00.0")
        check(rc == 0, "on failed: " + text)
        check("[ZrnSelectiveSuspend] Resuming device" in screen() and "is powered on" in screen(),
              "the console does not show the device coming back")
        check(active() == before, f"the screen did not return to {before}; it is on {active()}")
        check(open(f"{fake}/power").read() == "1", "the device was not powered on")
    finally:
        d.stop()


def frozen(pid):
    try:
        cgroup = open(f"/proc/{pid}/cgroup").read().strip().split("::", 1)[1]
        return f"zss-parked-{pid}" in cgroup and "frozen 1" in open(f"/sys/fs/cgroup{cgroup}/cgroup.events").read()
    except OSError:
        return False


def process_outside_the_layer_is_frozen_while_off():
    """`zssctl off` freezes a process the layer cannot move, and `on` lets it carry on."""
    fake = "/tmp/fake-freeze"
    os.makedirs(fake, exist_ok=True)
    counter = f"{fake}/ticks"
    outsider = subprocess.Popen([sys.executable, "-c",
                                 f"import time\nn = 0\nwhile True:\n    n += 1\n    open('{counter}', 'w').write(str(n))\n    time.sleep(0.05)"])
    for name, text in (("power", "1"), ("wake", "0"), ("holders", str(outsider.pid))):
        with open(f"{fake}/{name}", "w") as f:
            f.write(text)
    ticks = lambda: int(open(counter).read() or 0)
    d = Daemon("--gpu", "ffff:00:00.0=fake", "--stop-services", "", runtime_dir="/run",
               daemon_env=dict(os.environ, ZSSD_FAKE_DIR=fake))
    try:
        time.sleep(0.5)
        rc, text = d.ctl("off", "ffff:00:00.0")
        check(rc == 0 and f"processes frozen: python" in text and f"(pid {outsider.pid})" in text, "off did not freeze: " + text)
        check(frozen(outsider.pid) and open(f"{fake}/power").read().strip() == "0", "not frozen, or power not cut")
        seen = ticks()
        time.sleep(1.0)
        check(ticks() == seen, "the frozen process kept running")

        # A killed daemon must not leave it frozen: recovery thaws it with the device.
        d.proc.kill()
        d.proc.wait()
        subprocess.run([ZSSD, "--config", d.conf, "--runtime-dir", d.dir, "--gpu", "ffff:00:00.0=fake", "--stop-services", "",
                        "--recover"], env=dict(os.environ, ZSSD_FAKE_DIR=fake), check=False)
        wait_for(lambda: ticks() > seen, "the process to run again after recovery", 5)
        check(not frozen(outsider.pid) and open(f"{fake}/power").read().strip() == "1", "recovery left it frozen or off")
    finally:
        outsider.kill()
        outsider.wait()
        d.stop()


def device_is_hidden_while_off():
    """Switched off on request, the device's files read as empty; they are back after on, and after a kill."""
    fake = "/tmp/fake-hide"
    os.makedirs(fake, exist_ok=True)
    manifest = f"{fake}/driver icd.json"
    for name, text in (("power", "1"), ("wake", "0"), ("driver icd.json", '{"ICD": {"library_path": "libfake.so"}}')):
        with open(f"{fake}/{name}", "w") as f:
            f.write(text)
    shown = lambda: open(manifest).read()
    args = ("--gpu", "ffff:00:00.0=fake", "--stop-services", "", "--hide-while-off", manifest)
    env = dict(os.environ, ZSSD_FAKE_DIR=fake)
    d = Daemon(*args, runtime_dir="/run", daemon_env=env)
    try:
        rc, text = d.ctl("off", "ffff:00:00.0")
        check(rc == 0 and "Hidden from new programs (1)" in text and manifest in text, "nothing was hidden: " + text)
        check(shown() == "", "the file is still readable while the device is off")
        rc, text = d.ctl("on", "ffff:00:00.0")
        check(rc == 0 and "libfake.so" in shown(), "the file did not come back with the device")

        # The idle timer hides nothing: a device it switched off comes back for whoever asks.
        check(d.ctl("off", "ffff:00:00.0")[0] == 0 and shown() == "", "second off did not hide")
        d.proc.kill()
        d.proc.wait()
        check(shown() == "", "the mount went away with the daemon, which is not what is being tested")
        subprocess.run([ZSSD, "--config", d.conf, "--runtime-dir", d.dir, *args, "--recover"], env=env, check=False)
        check("libfake.so" in shown(), "recovery left the file hidden")
        check(open(f"{fake}/power").read().strip() == "1", "recovery left the device off")
    finally:
        d.stop()


if __name__ == "__main__":
    sys.stdout = open(f"{WORK}/results.txt", "w", buffering=1)
    CARD = find_card()
    if not CARD:
        print("FAIL  no bochs-display card in the guest")
        sys.exit(1)
    sys.exit(run_scenarios([
        ("idle card: detach to safe-to-remove, explicit attach", idle_detach_then_explicit_attach),
        ("a returning card is attached without a command", returning_card_is_attached_automatically),
        ("application migrates away, card leaves and returns, application follows", application_follows_the_card),
        ("a process outside the layer blocks the detach", process_outside_the_layer_blocks),
        ("a failed power-off never yields safe-to-remove", failed_power_off_is_never_safe_to_remove),
        ("application with nowhere to go is parked and frozen, then resumes", application_with_nowhere_to_go_is_parked_and_frozen),
        ("pulled card: lost within two seconds, detach refused, returns", pulled_card_is_reported_lost),
        ("pulled card: application evacuated, stale handle listed, follows the card back", pulled_card_evacuates_its_application),
        ("pulled card with nowhere to go: application parks, then resumes", pulled_card_with_nowhere_to_go_parks),
        ("off freezes a process outside the layer; recovery thaws it", process_outside_the_layer_is_frozen_while_off),
        ("a device switched off on request is hidden from new programs", device_is_hidden_while_off),
        ("off --console shows each step on the text console and returns", progress_is_shown_on_the_console),
    ]))
