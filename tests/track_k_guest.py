#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Runs inside the Track K guest: exercises zss.ko against the virtual card."""
import errno
import os
import select
import socket
import subprocess
import sys
import time

BUILD = os.environ["ZSS_BUILD"]
sys.path.insert(0, os.path.join(BUILD, "..", "tests"))
from track_b_guest import WORK, driver_of, find_card, host, wait_for  # noqa: E402
from zsstest import Failure, check, run_scenarios  # noqa: E402

KO = os.path.join(BUILD, "kmod", "zss.ko")
ROOT = "/sys/kernel/zss"
CARD = None


def read(path):
    with open(path) as f:
        return f.read().strip()


def write(path, text):
    """Writes to a sysfs file; returns 0 or the errno the kernel answered with."""
    try:
        fd = os.open(path, os.O_WRONLY)
    except OSError as e:
        return e.errno
    try:
        os.write(fd, text.encode())
        return 0
    except OSError as e:
        return e.errno
    finally:
        os.close(fd)


def dev(name):
    return f"{ROOT}/{CARD}/{name}"


def config():
    with open(f"/sys/bus/pci/devices/{CARD}/config", "rb") as f:
        return f.read(64)


def loaded():
    return os.path.isdir(ROOT)


def load():
    if not loaded():
        r = subprocess.run(["insmod", KO], capture_output=True, text=True)
        check(r.returncode == 0, "insmod failed: " + r.stderr)


def unload():
    if loaded():
        r = subprocess.run(["rmmod", "zss"], capture_output=True, text=True)
        check(r.returncode == 0, "rmmod failed: " + r.stderr)


def manage(options="backend=test quiesce=pm"):
    load()
    if os.path.isdir(f"{ROOT}/{CARD}"):
        check(write(f"{ROOT}/unmanage", CARD) == 0, "could not unmanage before a test")
    return write(f"{ROOT}/manage", f"{CARD} {options}")


class Events:
    """Kernel events from the module, as dictionaries."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_NETLINK, socket.SOCK_DGRAM, 15)
        self.sock.bind((0, 1))

    def next(self, timeout=3.0, **want):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not select.select([self.sock], [], [], max(0.0, deadline - time.time()))[0]:
                break
            fields = dict(p.split("=", 1) for p in self.sock.recv(8192).decode(errors="replace").split("\0") if "=" in p)
            if fields.get("SUBSYSTEM") == "zss" and all(fields.get(k) == v for k, v in want.items()):
                return fields
        raise Failure(f"no event {want} within {timeout} s")

    def close(self):
        self.sock.close()


def kernel_complaints():
    """Warnings, oopses and the like in the kernel log since boot."""
    bad = []
    fd = os.open("/dev/kmsg", os.O_RDONLY | os.O_NONBLOCK)
    try:
        while True:
            try:
                line = os.read(fd, 8192).decode(errors="replace")
            except BlockingIOError:
                break
            except OSError as e:
                if e.errno == errno.EPIPE:
                    continue
                raise
            text = line.split(";", 1)[-1]
            if any(w in text for w in ("WARNING:", "BUG:", "Oops", "Call Trace", "general protection", "refcount_t")):
                bad.append(text.strip())
    finally:
        os.close(fd)
    return bad


# ---- scenarios ------------------------------------------------------------------------


def loading_changes_nothing():
    before = config()
    load()
    check(read(f"{ROOT}/version") and sorted(os.listdir(ROOT)) == ["manage", "unmanage", "version"],
          f"unexpected contents after loading: {os.listdir(ROOT)}")
    check(config() == before and driver_of(CARD) == "bochs-drm", "loading the module touched the card")


def manage_and_unmanage():
    ev = Events()
    try:
        check(manage() == 0, "manage failed")
        ev.next(ACTION="add")
        check(read(dev("state")) == "on" and read(dev("backend")) == "test" and read(dev("quiesce")) == "pm", "wrong files")
        check(read(dev("functions")) == f"{CARD} bochs-drm online", "functions: " + read(dev("functions")))
        check(read(dev("iommu")) in ("yes", "no") and read(dev("cycles")) == "0", "iommu or cycles unreadable")
        check(manage() == 0, "managing again after unmanaging failed")
        check(write(f"{ROOT}/manage", f"{CARD} backend=test") == errno.EEXIST, "a device was managed twice")
        check(write(f"{ROOT}/unmanage", CARD) == 0 and not os.path.isdir(f"{ROOT}/{CARD}"), "unmanage left the directory")
        check(driver_of(CARD) == "bochs-drm", "the card lost its driver")
    finally:
        ev.close()


def bad_requests_are_refused():
    load()
    check(write(f"{ROOT}/manage", "0000:1f:1f.7 backend=test") == errno.ENODEV, "an absent device was accepted")
    check(write(f"{ROOT}/manage", "nonsense") == errno.EINVAL, "nonsense was accepted")
    check(write(f"{ROOT}/manage", f"{CARD} backend=nosuch") == errno.EINVAL, "an unknown backend was accepted")
    check(write(f"{ROOT}/manage", f"{CARD} quiesce=nosuch") == errno.EINVAL, "an unknown strategy was accepted")
    check(write(f"{ROOT}/unmanage", "0000:1f:1f.7") == errno.ENODEV, "unmanaging an unknown device succeeded")
    # No real power backend exists for a card in this virtual machine.
    rc = write(f"{ROOT}/manage", CARD)
    if rc == 0:
        backend = read(dev("backend"))
        write(f"{ROOT}/unmanage", CARD)
        return f"a real backend ({backend}) claimed the virtual card, so the refusal could not be checked"
    check(rc == errno.EOPNOTSUPP and os.listdir(ROOT) == ["manage", "unmanage", "version"] or
          sorted(os.listdir(ROOT)) == ["manage", "unmanage", "version"], f"manage without a backend: errno {rc}")
    check(rc == errno.EOPNOTSUPP, f"manage without a backend answered errno {rc}")


def cycle(options, expect_d3):
    before = config()
    ev = Events()
    try:
        check(manage(options) == 0, "manage failed")
        check(write(dev("power"), "off") == 0, "off failed: " + read(dev("last_error")))
        ev.next(ZSS_STATE="off", ZSS_PCI=CARD)
        check(read(dev("state")) == "off" and read(dev("power")) == "off", "state is not off")
        pstate = read(f"/sys/bus/pci/devices/{CARD}/power_state")
        check((pstate != "D0") == expect_d3, f"the PCI core reports {pstate} while off")
        # Marked disconnected: the kernel and well-behaved drivers leave it alone.
        check(read(dev("functions")).endswith("offline"), "the device is not marked disconnected while off")
        check(write(dev("power"), "off") == 0, "a second off was refused")

        check(write(dev("power"), "on") == 0, "on failed: " + read(dev("last_error")))
        ev.next(ZSS_STATE="on", ZSS_PCI=CARD)
        check(read(dev("state")) == "on" and read(dev("cycles")) == "1", "state or cycle count wrong after on")
        check(read(dev("functions")).endswith("online"), "the device is still marked disconnected after on")
        check(config() == before, "the PCI configuration differs after the cycle")
        check(read(f"/sys/bus/pci/devices/{CARD}/power_state") == "D0", "the card is not in D0 after on")
        check(driver_of(CARD) == "bochs-drm", "the driver is gone after the cycle")
        # The driver answers again: ask it to probe its connector.
        for status in [p for p in os.listdir("/sys/class/drm") if p.startswith("card") and "-" in p]:
            path = f"/sys/class/drm/{status}/status"
            if os.path.realpath(f"/sys/class/drm/{status}/device/device").endswith(CARD):
                check(write(path, "detect") == 0 and read(path) in ("connected", "disconnected", "unknown"),
                      "the driver does not answer after the cycle")
        for _ in range(5):
            check(write(dev("power"), "off") == 0 and write(dev("power"), "on") == 0, "a repeated cycle failed")
        check(config() == before and read(dev("cycles")) == "6", "state drifted over repeated cycles")
    finally:
        ev.close()
        write(f"{ROOT}/unmanage", CARD)


def cycle_with_the_drivers_sleep_callbacks():
    cycle("backend=test quiesce=pm", expect_d3=True)


def cycle_without_quiesce():
    cycle("backend=test quiesce=none", expect_d3=False)
    cycle("backend=test quiesce=external", expect_d3=False)


def failures_leave_the_device_on():
    before = config()
    check(manage() == 0, "manage failed")
    try:
        check(write(f"{ROOT}/manage", f"{CARD} backend=test") == errno.EEXIST, "managed twice")
        # The backend cannot cut power.
        check(write(dev("test_fault"), "1") == 0, "could not set a fault")
        check(write(dev("power"), "off") == errno.EIO, "a failed power cut was reported as success")
        check(read(dev("state")) == "on" and "could not cut power" in read(dev("last_error")), read(dev("last_error")))
        check(config() == before and read(f"/sys/bus/pci/devices/{CARD}/power_state") == "D0", "the card was not put back")
        # The backend says it did, and the card still answers.
        write(dev("test_fault"), "2")
        check(write(dev("power"), "off") == errno.EBUSY, "a power cut that did not happen was accepted")
        check(read(dev("state")) == "on" and "still answers" in read(dev("last_error")), read(dev("last_error")))
        check(config() == before, "the card was not put back")
        # The card does not come back.
        write(dev("test_fault"), "4")
        check(write(dev("power"), "off") == 0, "off failed")
        t = time.time()
        check(write(dev("power"), "on") == errno.ETIMEDOUT, "a card that did not return was reported as on")
        check(time.time() - t < 6 and read(dev("state")) == "failed" and "did not answer" in read(dev("last_error")),
              "wrong state after no return: " + read(dev("state")))
        check(write(dev("power"), "off") == errno.EINVAL, "off was accepted in the failed state")
        write(dev("test_fault"), "0")
        check(write(dev("power"), "on") == 0 and read(dev("state")) == "on", "a retry did not bring it back")
        check(config() == before and read(dev("last_error")) == "", "the card or the error text was not restored")
    finally:
        write(dev("test_fault"), "0")
        write(f"{ROOT}/unmanage", CARD)


def silence_is_noticed():
    before = config()
    check(manage("backend=test quiesce=none") == 0, "manage failed")
    ev = Events()
    try:
        t = time.time()
        write(dev("test_fault"), "8")
        ev.next(1.0, ZSS_STATE="lost", ZSS_PCI=CARD)
        took = time.time() - t
        check(took < 0.35, f"the loss took {took * 1000:.0f} ms to notice")
        check(read(dev("state")) == "lost", "state is not lost")
        check(read(dev("functions")).endswith("offline"), "the lost device is not marked disconnected")
        check(write(dev("power"), "on") == errno.ETIMEDOUT and read(dev("state")) == "lost", "a silent device was taken back")
        write(dev("test_fault"), "0")
        check(write(dev("power"), "on") == 0 and read(dev("state")) == "on", "a returned device was not taken back")
        check(config() == before, "the configuration of the returned device differs")

        # A device that is off is expected to be silent.
        check(write(dev("power"), "off") == 0, "off failed")
        write(dev("test_fault"), "8")
        time.sleep(0.5)
        check(read(dev("state")) == "off", "silence while off was reported as a loss")
        write(dev("test_fault"), "0")
        check(write(dev("power"), "on") == 0, "on failed")
    finally:
        ev.close()
        write(dev("test_fault"), "0")
        write(f"{ROOT}/unmanage", CARD)


def a_pulled_card_is_reported():
    global CARD
    check(manage() == 0, "manage failed")
    ev = Events()
    try:
        check(host("unplug") in ("ok", "gone"), "the host could not pull the card")
        ev.next(20.0, ZSS_STATE="lost", ZSS_PCI=CARD)
        check(read(dev("state")) == "lost", "state is not lost after the card was pulled")
        check(write(dev("power"), "on") == errno.ENODEV and "left the bus" in read(dev("last_error")),
              "a pulled card was powered on: " + read(dev("last_error")))
        check(write(f"{ROOT}/unmanage", CARD) == 0, "a pulled card could not be unmanaged")
    finally:
        ev.close()
        check(host("plug") == "ok", "the host could not return the card")
        wait_for(lambda: find_card() and driver_of(find_card()), "the card and its driver to return", 30)
        CARD = find_card()
    check(manage() == 0 and read(dev("state")) == "on", "the returned card could not be managed")
    check(write(dev("power"), "off") == 0 and write(dev("power"), "on") == 0, "the returned card does not cycle")
    write(f"{ROOT}/unmanage", CARD)


def driver_is_frozen_on_loss_and_thawed_on_return():
    """A driver that offers the freeze hooks is frozen the moment its device goes silent, and thawed when it is back."""
    hooks = os.path.join(BUILD, "kmod", "zss_test_hooks.ko")
    par = "/sys/module/zss_test_hooks/parameters/"
    unload()
    check(subprocess.run(["insmod", hooks]).returncode == 0, "the stand-in module did not load")
    check(subprocess.run(["insmod", KO, "freeze_any=1"]).returncode == 0, "zss.ko did not load with freeze_any")
    try:
        check(write(f"{ROOT}/manage", f"{CARD} backend=test quiesce=none") == 0, "manage failed")
        ev = Events()
        try:
            write(dev("test_fault"), "8")
            lost = ev.next(1.0, ZSS_STATE="lost", ZSS_PCI=CARD)
        finally:
            ev.close()
        check("driver frozen" in lost.get("ZSS_REASON", ""), "the event does not say the driver was frozen: " + str(lost))
        check(read(par + "freezes") == "1" and read(par + "frozen") == "1" and read(dev("driver_frozen")) == "1",
              "the driver was not frozen on the loss")
        # A resume that fails leaves the driver frozen and the device lost, and can be tried again.
        write(dev("test_fault"), "0")
        write(par + "fail_thaw", "1")
        check(write(dev("power"), "on") == errno.EIO and read(dev("state")) == "lost" and read(par + "frozen") == "1",
              "a failed thaw was not reported, or the driver was let go")
        check("did not resume" in read(dev("last_error")), read(dev("last_error")))
        write(par + "fail_thaw", "0")
        check(write(dev("power"), "on") == 0 and read(dev("state")) == "on", "the device was not taken back")
        check(read(par + "thaws") == "1" and read(par + "frozen") == "0" and read(dev("driver_frozen")) == "0",
              "the driver was not thawed")
        check(read(dev("needs_rebind")) == "0", "a thawed driver was reported as needing a rebind")
        # An orderly off and on does not involve the freeze at all.
        check(write(dev("power"), "off") == 0 and write(dev("power"), "on") == 0, "cycle failed")
        check(read(par + "freezes") == "1" and read(par + "thaws") == "1", "an orderly cycle froze the driver")
    finally:
        write(dev("test_fault"), "0")
        write(f"{ROOT}/unmanage", CARD)
        unload()
        subprocess.run(["rmmod", "zss_test_hooks"])


# ---- the daemon on top of the module ---------------------------------------------------


def daemon_state(d):
    rc, out = d.ctl("status", CARD)
    check(rc == 0 and "state=" in out, "status failed: " + out)
    return out.split("state=")[1].split()[0], out


def daemon_uses_the_module():
    from zsstest import Daemon
    load()
    before = config()
    d = Daemon("--kmod-backend", "test", "--gpu", CARD, "--no-auto-attach", runtime_dir="/run")
    try:
        state, text = daemon_state(d)
        check("backend=zss-kmod" in text, "the daemon did not pick the kernel module: " + text)
        check(read(dev("quiesce")) == "pm" and read(dev("backend")) == "test", "the module was told the wrong thing")
        rc, text = d.ctl("detach", CARD)
        check(rc == 0, "detach failed: " + text)
        check(daemon_state(d)[0] == "powered-off" and read(dev("state")) == "off", "the device is not off in the module")
        check(read(f"/sys/bus/pci/devices/{CARD}/power_state") != "D0", "the driver's sleep callbacks did not run")
        rc, text = d.ctl("attach", CARD)
        check(rc == 0 and daemon_state(d)[0] == "attached" and read(dev("state")) == "on", "attach failed: " + text)
        check(config() == before and driver_of(CARD) == "bochs-drm", "the card is not as it was")

        # A failure in the kernel reaches the user with the module's own words.
        write(dev("test_fault"), "1")
        rc, text = d.ctl("detach", CARD)
        check(rc != 0 and "could not cut power" in text and daemon_state(d)[0] == "attached", "failure not reported: " + text)
        write(dev("test_fault"), "0")
    finally:
        d.stop()
        write(dev("test_fault"), "0")
        write(f"{ROOT}/unmanage", CARD)


def daemon_hears_of_a_loss_from_the_module():
    from zsstest import Daemon
    load()
    d = Daemon("--kmod-backend", "test", "--gpu", CARD, runtime_dir="/run")
    try:
        check(daemon_state(d)[0] == "attached", "not attached at the start")
        t = time.time()
        write(dev("test_fault"), "8")
        wait_for(lambda: daemon_state(d)[0] == "lost", "the daemon to report the loss", 5)
        took = time.time() - t
        check(took < 1.0, f"the daemon took {took:.2f} s to report a loss the module saw at once")
        time.sleep(1.5)
        check(daemon_state(d)[0] == "lost", "a silent device was attached again")
        # Asking for it while it is still silent fails, in the module's words, and changes nothing.
        rc, text = d.ctl("on", CARD)
        check(rc != 0 and "did not answer" in text and daemon_state(d)[0] == "lost", "on for a silent device: " + text)
        # It answers again: the daemon takes it back without a command and rebinds the driver.
        write(dev("test_fault"), "0")
        wait_for(lambda: daemon_state(d)[0] == "attached", "the daemon to take the device back", 10)
        check(read(dev("state")) == "on" and driver_of(CARD) == "bochs-drm", "the device or its driver is not back")
        check(d.ctl("detach", CARD)[0] == 0 and d.ctl("attach", CARD)[0] == 0, "no cycle after the return")
    finally:
        d.stop()
        if os.path.isdir(f"{ROOT}/{CARD}"):
            write(dev("test_fault"), "0")
            write(f"{ROOT}/unmanage", CARD)


def unloading_restores_power():
    before = config()
    check(manage() == 0 and write(dev("power"), "off") == 0, "could not power the card off")
    unload()
    check(not loaded(), "the module is still there")
    check(config() == before and read(f"/sys/bus/pci/devices/{CARD}/power_state") == "D0", "the card was left off")
    check(driver_of(CARD) == "bochs-drm", "the driver is gone")
    load()
    check(manage() == 0 and write(dev("power"), "off") == 0 and write(dev("power"), "on") == 0, "no cycle after a reload")
    unload()


def the_kernel_did_not_complain():
    bad = kernel_complaints()
    check(not bad, "the kernel log has: " + " | ".join(bad[:4]))


if __name__ == "__main__":
    sys.stdout = open(f"{WORK}/results.txt", "w", buffering=1)
    CARD = find_card()
    if not CARD or not os.path.exists(KO):
        print("FAIL  no bochs-display card in the guest, or no zss.ko")
        sys.exit(1)
    wait_for(lambda: driver_of(CARD), "the bochs driver to bind", 20)
    sys.exit(run_scenarios([
        ("loading the module changes nothing", loading_changes_nothing),
        ("manage and unmanage", manage_and_unmanage),
        ("bad requests are refused", bad_requests_are_refused),
        ("off and on through the driver's sleep callbacks", cycle_with_the_drivers_sleep_callbacks),
        ("off and on without touching the driver", cycle_without_quiesce),
        ("a failure at any step leaves the device on, or says it is not", failures_leave_the_device_on),
        ("a device that goes silent is noticed and can be taken back", silence_is_noticed),
        ("a pulled card is reported and its return can be managed", a_pulled_card_is_reported),
        ("a driver with freeze hooks is frozen on a loss and thawed on the return", driver_is_frozen_on_loss_and_thawed_on_return),
        ("the daemon powers a device off and on through the module", daemon_uses_the_module),
        ("the daemon hears of a loss from the module and takes the device back", daemon_hears_of_a_loss_from_the_module),
        ("unloading the module restores power", unloading_restores_power),
        ("the kernel log is clean", the_kernel_did_not_complain),
    ]))
