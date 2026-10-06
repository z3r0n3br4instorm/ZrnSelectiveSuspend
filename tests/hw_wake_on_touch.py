#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Track C: wake-on-touch with the patched NVIDIA driver.

REAL HARDWARE, ROOT ONLY, Apple classic-gmux laptops only, and only with the
driver built from patches/nvidia-470.256.02-wake-on-touch.patch (it refuses
to run otherwise).

Suspends the NVIDIA driver and cuts dGPU power, then leaves the X session on
screen with the card off. With the patch, anything that calls into the
suspended driver bumps /sys/module/nvidia/parameters/zss_wake_requests and
sleeps; this script watches that counter and, when it moves, powers the card
on and resumes the driver, which lets the caller continue.

Modes:
  vt    switch to the text console for the suspend, then back to the desktop
        with the card off (the sequence that used to freeze X)
  live  never leave the desktop: suspend and cut power under the running X,
        then run display queries to see which of them wakes the card
  idle  as live, but run no queries: shows how long the card stays off when
        only the desktop itself and whatever the user does can wake it

Usage: sudo python3 hw_wake_on_touch.py LOGFILE vt|live|idle [MAX_SECONDS_OFF]

A watchdog process powers the card back on after MAX_SECONDS_OFF + 30 s
whatever else happens.
"""
import os, signal, subprocess, sys, threading, time

LOG, MODE = sys.argv[1], sys.argv[2]
MAX_OFF = float(sys.argv[3]) if len(sys.argv) > 3 else 60
GPU, AUDIO = "0000:01:00.0", "0000:01:00.1"
CFG = f"/sys/bus/pci/devices/{GPU}/config"
COUNTER = "/sys/module/nvidia/parameters/zss_wake_requests"
CONSOLE = "/dev/tty63"
log = open(LOG, "a")
t0 = time.time()


def say(msg, screen=False):
    log.write("[%7.2f] %s\n" % (time.time() - t0, msg)); log.flush(); os.fsync(log.fileno())
    if screen:
        try:
            with open(CONSOLE, "w") as tty:
                tty.write("\r\n  ZSS test: %s\r\n" % msg)
        except OSError:
            pass


def sh(cmd, timeout=20, env=None):
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout, env=env)
        return r.returncode, (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        return 124, "timed out after %d s" % timeout


def port(p, val=None):
    fd = os.open("/dev/port", os.O_RDWR)
    try:
        if val is None:
            return os.pread(fd, 1, p)[0]
        os.pwrite(fd, bytes([val]), p)
    finally:
        os.close(fd)


def vendor():
    with open(CFG, "rb") as f:
        b = f.read(2)
    return b[0] | b[1] << 8


def counter():
    with open(COUNTER) as f:
        return int(f.read())


def write_file(path, text):
    try:
        with open(path, "w") as f:
            f.write(text)
        return None
    except OSError as e:
        return str(e)


def driver(dev):
    p = f"/sys/bus/pci/devices/{dev}/driver"
    return os.path.basename(os.path.realpath(p)) if os.path.exists(p) else None


def power_on_and_resume(saved):
    """Returns a description of what happened; safe to call when already on."""
    if port(0x750) == 0 or vendor() == 0xffff:
        port(0x750, 1); port(0x750, 3)
        for _ in range(50):
            time.sleep(0.1)
            if vendor() != 0xffff:
                break
        time.sleep(0.3)
        fd = os.open(CFG, os.O_WRONLY)
        os.pwrite(fd, saved[0x10:], 0x10); os.pwrite(fd, saved[0x0c:0x10], 0x0c); os.pwrite(fd, saved[0x04:0x06], 0x04)
        os.close(fd)
    err = write_file("/proc/driver/nvidia/suspend", "resume")
    return "gmux 0x%02x, vendor 0x%04x, resume %s" % (port(0x750), vendor(), err or "ok")


def x_env():
    pid = sh("pgrep -x Xorg")[1].split("\n")[0]
    args = open(f"/proc/{pid}/cmdline").read().split("\0")
    auth = args[args.index("-auth") + 1] if "-auth" in args else ""
    display = next((a for a in args if a.startswith(":")), ":0")
    return pid, dict(os.environ, DISPLAY=display, XAUTHORITY=auth)


def x_check(label, cmd, timeout=20):
    t = time.time()
    rc, out = sh(cmd, timeout, xenv)
    say("X check %-16s %s in %.2f s%s" % (label + ":", "ok" if rc == 0 else "FAILED (rc %d)" % rc, time.time() - t,
                                           "" if rc == 0 else " -- " + out[-160:].replace("\n", " | ")))
    return rc == 0


def blocked_in_driver():
    """Which of the processes holding the card are waiting inside the NVIDIA driver right now."""
    found = []
    for pid in sh("pgrep -x Xorg; pgrep -x nvidia-persiste")[1].split():
        try:
            stack = open(f"/proc/{pid}/stack").read()
        except OSError:
            continue
        if "nvidia" in stack or "nvkms" in stack or "rwsem_down_read" in stack:
            frames = [l.split()[1].split("+")[0] for l in stack.strip().split("\n") if len(l.split()) > 1][:7]
            found.append("%s(%s): %s" % (open(f"/proc/{pid}/comm").read().strip(), pid, " < ".join(frames)))
    return found


if not os.path.exists(COUNTER):
    sys.exit("the running nvidia module is not the wake-on-touch build; refusing to run")

say("=== wake-on-touch, mode %s, at most %.0f s off ===" % (MODE, MAX_OFF))
xpid, xenv = x_env()
vt = sh("fgconsole")[1]
say("Xorg pid %s on VT %s; gmux 0x%02x; vendor 0x%04x; counter %d" % (xpid, vt, port(0x750), vendor(), counter()))
dmesg_mark = sh("dmesg | tail -1")[1]
audio_drv = driver(AUDIO)
with open(CFG, "rb") as f:
    saved = f.read(256)

# Independent safety net: whatever happens to this process, the card comes back.
watchdog = os.fork()
if watchdog == 0:
    signal.signal(signal.SIGTERM, lambda *_: os._exit(0))
    time.sleep(MAX_OFF + 30)
    say("WATCHDOG: forcing the dGPU on: " + power_on_and_resume(saved), screen=True)
    sh("chvt " + vt, 10)
    os._exit(0)

suspended = False
woke_at = None
try:
    x_check("before", "xset q")
    say("stop nvidia-persistenced: rc=%d %s" % sh("systemctl stop nvidia-persistenced"))
    if MODE == "vt":
        say("chvt 63: rc=%d %s" % sh("chvt 63", 10)); time.sleep(1.0)
        say("suspending the NVIDIA driver and cutting dGPU power", screen=True)
    if audio_drv:
        say("unbind audio: %s" % (write_file(f"/sys/bus/pci/devices/{AUDIO}/driver/unbind", AUDIO) or "ok"))
    base = counter()
    t = time.time(); err = write_file("/proc/driver/nvidia/suspend", "suspend")
    say("nvidia suspend%s: %s (%.2f s)" % (" with X on screen" if MODE != "vt" else "", err or "ok", time.time() - t))
    if err:
        raise RuntimeError("driver would not suspend; power not cut")
    suspended = True
    port(0x750, 1); port(0x750, 0); time.sleep(0.3)
    off_at = time.time()
    say("gmux power off: port 0x%02x, vendor 0x%04x, counter %d" % (port(0x750), vendor(), counter()))

    if MODE == "vt":
        say("returning to the desktop with the dGPU off", screen=True)
        rc, out = sh("chvt " + vt, 15)
        say("chvt %s with the dGPU off: rc=%d %s" % (vt, rc, out))

    # Poke X the way a desktop does, without waiting for the answers: any of these
    # may sleep inside the driver until the card is back.
    probes = [(5, "xset q"), (12, "xrandr --listproviders"), (20, "xrandr --query"), (30, "xdpyinfo")]
    started = set()
    while time.time() - off_at < MAX_OFF:
        now = counter()
        if now != base:
            woke_at = time.time() - off_at
            say("WAKE REQUEST: counter %d -> %d, %.2f s after power-off" % (base, now, woke_at))
            for line in blocked_in_driver():
                say("  waiting in the driver: " + line)
            break
        if MODE == "live":
            for at, cmd in probes:
                if cmd not in started and time.time() - off_at >= at:
                    started.add(cmd)
                    say("probe started: %s (counter %d)" % (cmd, now))
                    threading.Thread(target=x_check, args=("probe " + cmd.split()[-1], cmd, 40), daemon=True).start()
        time.sleep(0.02)
    if woke_at is None:
        say("no wake request in %.0f s: nothing touched the driver while the dGPU was off" % MAX_OFF)
finally:
    if suspended:
        t = time.time()
        try:
            say("power on and resume: %s (%.2f s)" % (power_on_and_resume(saved), time.time() - t))
        except Exception as e:
            say("ERROR during power-on/resume: %r" % e, screen=True)
    if audio_drv:
        say("rebind audio: %s" % (write_file(f"/sys/bus/pci/drivers/{audio_drv}/bind", AUDIO) or "ok"))
    try:
        os.kill(watchdog, signal.SIGTERM)
    except OSError:
        pass
    say("start nvidia-persistenced: rc=%d %s" % sh("systemctl start nvidia-persistenced"))
    time.sleep(2.0)
    alive = os.path.exists(f"/proc/{xpid}")
    ok = alive and x_check("after", "xset q")
    if ok:
        x_check("after providers", "xrandr --listproviders")
        x_check("after outputs", "xrandr --query", 30)
    say("after: gmux 0x%02x vendor 0x%04x drivers %s / %s; counter %d; active VT %s" %
        (port(0x750), vendor(), driver(GPU), driver(AUDIO), counter(), sh("fgconsole")[1]))
    out = sh("dmesg")[1]
    new = out.split(dmesg_mark)[-1] if dmesg_mark in out else out[-3000:]
    lines = [l for l in new.strip().split("\n") if "input:" not in l and "snd_hda" not in l]
    say("--- kernel log during the test (audio lines left out) ---\n" + ("\n".join(lines) or "(nothing)"))
    if not ok:
        sh("chvt 63", 5)
        say("ERROR: the X session %s. The dGPU is powered on again." % ("is not answering" if alive else "EXITED"), screen=True)
        say("Press Ctrl+Alt+F%s to return to it, or Ctrl+Alt+F3 for a login console. Log: %s" % (vt, LOG), screen=True)
    say("=== done: X %s; wake request %s ===" % ("fine" if ok else "NOT OK",
        "after %.2f s" % woke_at if woke_at is not None else "never came"))
