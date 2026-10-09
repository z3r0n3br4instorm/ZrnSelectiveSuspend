#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Runs inside the Track V guest: lending a card and taking it back."""
import json
import os
import shutil
import subprocess
import sys
import time

BUILD = os.environ["ZSS_BUILD"]
sys.path.insert(0, os.path.join(BUILD, "..", "tests"))
import track_k_guest as k  # noqa: E402
from track_b_guest import WORK, driver_of, wait_for  # noqa: E402
from zsstest import ZSSCTL, ZSSD, check, quiet_env, run_scenarios  # noqa: E402

CARD = None
PCI = "/sys/bus/pci/devices"


def card_on_the_port():
    """The display card behind the hot-plug port (the guest has a second one, on the root bus, for the host to keep)."""
    for dev in sorted(os.listdir(PCI)):
        try:
            behind_a_bridge = os.path.realpath(f"{PCI}/{dev}").count("/0000:") == 2
            if open(f"{PCI}/{dev}/vendor").read().strip() == "0x1234" and behind_a_bridge:
                return dev
        except OSError:
            pass
    return None


def group():
    return os.path.basename(os.readlink(f"{PCI}/{CARD}/iommu_group"))


def bind_back():
    """The card as a test should find it: on, managed by nobody, with its own driver."""
    if os.path.isdir(f"{k.ROOT}/{CARD}"):
        if k.read(k.dev("state")) == "lent":
            if driver_of(CARD):
                k.write(f"{PCI}/{CARD}/driver/unbind", CARD)
            k.write(k.dev("power"), "reclaim")
        k.write(f"{k.ROOT}/unmanage", CARD)
    k.write(f"{PCI}/{CARD}/driver_override", "\n")
    if driver_of(CARD) != "bochs-drm":
        if driver_of(CARD):
            k.write(f"{PCI}/{CARD}/driver/unbind", CARD)
        k.write("/sys/bus/pci/drivers_probe", CARD)
    wait_for(lambda: driver_of(CARD) == "bochs-drm", "the card's own driver to bind", 10)


class Guest:
    """Stands in for a virtual machine: a process that has the card's isolation group open."""

    def __init__(self):
        self.proc = subprocess.Popen([sys.executable, "-c",
                                      "import os,sys,time; os.open(sys.argv[1], os.O_RDWR); print('held', flush=True); time.sleep(600)",
                                      f"/dev/vfio/{group()}"], stdout=subprocess.PIPE, text=True)
        check(self.proc.stdout.readline().strip() == "held", "the stand-in guest could not open the group")

    def stop(self):
        self.proc.kill()
        self.proc.wait()


class D:
    """zssd with a runtime directory that outlives it, so that it can be restarted."""

    def __init__(self, run):
        self.run = run
        os.makedirs(run, exist_ok=True)
        self.socket = os.path.join(run, "s")
        if os.path.exists(self.socket):
            os.unlink(self.socket)
        conf = os.path.join(run, "zssd.conf")
        open(conf, "w").close()
        self.log = open(os.path.join(run, "zssd.log"), "a+")
        self.proc = subprocess.Popen([ZSSD, "--config", conf, "--socket", self.socket, "--runtime-dir", run,
                                      "--kmod-backend", "test", "--gpu", CARD, "--no-auto-attach"],
                                     stdout=self.log, stderr=subprocess.STDOUT)
        wait_for(lambda: os.path.exists(self.socket), "zssd to start", 5)

    def ctl(self, *args):
        r = subprocess.run([ZSSCTL, *args], env=quiet_env({"ZSS_SOCKET": self.socket}), capture_output=True, text=True,
                           timeout=120)
        return r.returncode, r.stdout + r.stderr

    def state(self):
        rc, out = self.ctl("status", CARD)
        check(rc == 0 and "state=" in out, "status failed: " + out)
        return out.split("state=")[1].split()[0], out

    def stop(self):
        self.proc.terminate()
        self.proc.wait(10)
        self.log.close()


# ---- scenarios ------------------------------------------------------------------------


def the_guest_has_an_iommu():
    check(os.listdir("/sys/kernel/iommu_groups"), "no isolation groups: the emulated IOMMU is not in use")
    check(os.path.isdir("/sys/bus/pci/drivers/vfio-pci"), "the passthrough driver is not loaded")
    check(os.path.exists(f"{PCI}/{CARD}/iommu_group"), "the card is in no isolation group")


def module_lends_and_takes_back():
    """The module's half: the state, what it refuses while lent, the reset on the way back."""
    bind_back()
    check(k.manage("backend=test quiesce=none") == 0, "manage failed")
    before = k.config()
    cycles = int(k.read(k.dev("cycles")))
    check(k.write(k.dev("power"), "reclaim") != 0, "a device that is not lent was reclaimed")
    check(k.write(k.dev("power"), "lend") == 0 and k.read(k.dev("state")) == "lent", "lend failed: " + k.read(k.dev("last_error")))

    # Lent, it is not guarded: going silent is the guest's business.
    k.write(k.dev("test_fault"), "8")
    time.sleep(1.0)
    check(k.read(k.dev("state")) == "lent", "a lent device was reported " + k.read(k.dev("state")))
    k.write(k.dev("test_fault"), "0")
    check(k.write(k.dev("power"), "off") != 0 and k.write(k.dev("power"), "on") != 0, "a lent device took a power request")
    check(k.write(f"{k.ROOT}/unmanage", CARD) != 0 and "lent" in k.read(k.dev("last_error")), "a lent device was unmanaged")

    # Not reset under a driver.
    check(k.write(k.dev("power"), "reclaim") != 0 and "still bound" in k.read(k.dev("last_error")),
          "reclaimed under a bound driver: " + k.read(k.dev("last_error")))
    check(k.write(f"{PCI}/{CARD}/driver/unbind", CARD) == 0, "could not unbind the card's driver")

    # A reset that cannot be done leaves it lent.
    k.write(k.dev("test_fault"), "1")
    check(k.write(k.dev("power"), "reclaim") != 0 and k.read(k.dev("state")) == "lent", "a failed reset did not leave it lent")
    k.write(k.dev("test_fault"), "0")

    check(k.write(k.dev("power"), "reclaim") == 0 and k.read(k.dev("state")) == "on", "reclaim failed: " + k.read(k.dev("last_error")))
    check(int(k.read(k.dev("cycles"))) == cycles + 1, "the device was not power-cycled")
    check(k.write("/sys/bus/pci/drivers_probe", CARD) == 0, "probing failed")
    wait_for(lambda: driver_of(CARD) == "bochs-drm", "the card's driver to bind again", 10)
    check(k.config() == before, "the PCI configuration is not as it was")

    # A hand-over that is called off: no reset, the device is simply the host's again.
    cycles = int(k.read(k.dev("cycles")))
    check(k.write(k.dev("power"), "lend") == 0 and k.write(k.dev("power"), "unlend") == 0, "unlend failed")
    check(k.read(k.dev("state")) == "on" and int(k.read(k.dev("cycles"))) == cycles, "unlend was not a plain return")
    check(k.write(f"{k.ROOT}/unmanage", CARD) == 0, "unmanage failed")


def check_reports(d):
    rc, text = d.ctl("lend", "--check", CARD)
    rc2, raw = d.ctl("lend", "--check", CARD, "--json")
    doc = json.loads(raw)
    return rc, " | ".join(text.split("\n")), doc


def daemon_lends_and_reclaims():
    bind_back()
    run = os.path.join("/run", "zss-v")
    shutil.rmtree(run, ignore_errors=True)
    d = D(run)
    guest = None
    try:
        before = k.config()
        rc, text, doc = check_reports(d)
        check(rc == 0 and doc["can_lend"] and not doc["obstacles"], "the check found something in the way: " + text)
        check(any(f["pci"] == CARD and f["driver"] == "bochs-drm" for f in doc["functions"]), "the check did not list the card: " + text)
        check(d.state()[0] == "attached" and driver_of(CARD) == "bochs-drm", "the check changed something")

        rc, text = d.ctl("lend", CARD)
        check(rc == 0 and d.state()[0] == "lent", "lend failed: " + text)
        check(driver_of(CARD) == "vfio-pci" and k.read(k.dev("state")) == "lent", "the card was not handed over: " + text)
        for cmd in (("off", CARD), ("detach", CARD), ("on", CARD)):
            rc, text = d.ctl(*cmd)
            check(rc != 0 and "lent" in text and d.state()[0] == "lent", f"{cmd[0]} was not refused on a lent card: " + text)

        guest = Guest()
        state, text = d.state()
        check("guest" in text and str(guest.proc.pid) in text, "status does not name the holder: " + text)
        rc, text = d.ctl("reclaim", CARD)
        check(rc != 0 and str(guest.proc.pid) in text and d.state()[0] == "lent" and driver_of(CARD) == "vfio-pci",
              "reclaim was not refused while a guest holds the card: " + text)

        # The daemon restarted under a running guest: the card is still lent, and untouched.
        d.stop()
        d = D(run)
        check(d.state()[0] == "lent" and driver_of(CARD) == "vfio-pci", "the lent state did not survive a restart: " + d.state()[1])
        guest.stop()
        guest = None

        # A reset that fails leaves it lent; the next attempt works.
        k.write(k.dev("test_fault"), "1")
        rc, text = d.ctl("reclaim", CARD)
        check(rc != 0 and "reset" in text and d.state()[0] == "lent", "a failed reset was not reported: " + text)
        k.write(k.dev("test_fault"), "0")
        rc, text = d.ctl("reclaim", CARD)
        check(rc == 0 and d.state()[0] == "attached", "reclaim failed: " + text)
        wait_for(lambda: driver_of(CARD) == "bochs-drm", "the card's driver to bind again", 10)
        check(k.config() == before and k.read(k.dev("state")) == "on", "the card is not as it was")
        check(not os.path.exists(os.path.join(run, f"lent-{CARD}")), "the record of the loan was left behind")
    finally:
        if guest:
            guest.stop()
        d.stop()
        k.write(k.dev("test_fault"), "0")
        bind_back()


def a_hand_over_that_fails_is_undone():
    """With the passthrough driver unable to take the card, everything is put back."""
    bind_back()
    run = os.path.join("/run", "zss-v2")
    shutil.rmtree(run, ignore_errors=True)
    d = D(run)
    try:
        # The hand-over is made to fail after the card's driver has let go: it is offered to a driver that does not exist.
        os.environ["ZSS_TEST_PASS_DRIVER"] = "zss-no-such-driver"
        d.stop()
        d = D(run)
        rc, text = d.ctl("lend", CARD)
        check(rc != 0 and "binding the passthrough driver" in text and d.state()[0] == "attached",
              "a failed hand-over was not reported as one: " + " | ".join(text.split("\n")))
        wait_for(lambda: driver_of(CARD) == "bochs-drm", "the card's driver to be bound again", 10)
        check(k.read(k.dev("state")) == "on", "the module still thinks the card is lent")
        check(not os.path.exists(os.path.join(run, f"lent-{CARD}")), "the record of the loan was left behind")
    finally:
        os.environ.pop("ZSS_TEST_PASS_DRIVER", None)
        d.stop()
        bind_back()


def obstacles_are_all_reported():
    bind_back()
    run = os.path.join("/run", "zss-v3")
    shutil.rmtree(run, ignore_errors=True)
    d = D(run)
    holder = None
    try:
        # A program outside ZSS_AirLock with the card open, and the card switched off.
        node = next(os.path.join("/dev/dri", n) for n in sorted(os.listdir(f"{PCI}/{CARD}/drm")) if n.startswith("card"))
        holder = subprocess.Popen([sys.executable, "-c", "import os,sys,time; os.open(sys.argv[1], os.O_RDWR); print('h', flush=True); time.sleep(600)", node],
                                  stdout=subprocess.PIPE, text=True)
        holder.stdout.readline()
        rc, text, doc = check_reports(d)
        check(rc != 0 and not doc["can_lend"], "a program holding the card was not in the way: " + text)
        check(any(str(holder.pid) in o["reason"] and o["remedy"] for o in doc["obstacles"]), "the holder was not named: " + text)
        rc, text = d.ctl("lend", CARD)
        check(rc != 0 and "nothing was changed" in text and d.state()[0] == "attached" and driver_of(CARD) == "bochs-drm",
              "lend was not refused cleanly: " + text)
        holder.kill()
        holder.wait()
        holder = None
        rc, text, doc = check_reports(d)
        check(rc == 0 and doc["can_lend"], "the obstacle did not go away with the program: " + text)
    finally:
        if holder:
            holder.kill()
        d.stop()
        bind_back()


if __name__ == "__main__":
    sys.stdout = open(f"{WORK}/results.txt", "w", buffering=1)
    CARD = k.CARD = card_on_the_port()
    if not CARD or not os.path.exists(k.KO):
        print("FAIL  no bochs-display card in the guest, or no zss.ko")
        sys.exit(1)
    wait_for(lambda: driver_of(CARD), "the bochs driver to bind", 20)
    k.load()
    sys.exit(run_scenarios([
        ("the guest has an IOMMU and the passthrough driver", the_guest_has_an_iommu),
        ("the module lends a device, leaves it alone, and resets it on the way back", module_lends_and_takes_back),
        ("the daemon lends a card, names its holder, survives a restart, and reclaims it", daemon_lends_and_reclaims),
        ("a hand-over that fails is undone", a_hand_over_that_fails_is_undone),
        ("everything in the way is reported, and lending is refused without changing anything", obstacles_are_all_reported),
        ("the kernel log is clean", k.the_kernel_did_not_complain),
    ]))
