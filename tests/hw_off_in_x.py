#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Track C experiment: is the X session usable while the dGPU is powered off?

REAL HARDWARE, ROOT ONLY, Apple classic-gmux laptops only.

Suspends the NVIDIA driver and cuts dGPU power on the text console, then
returns to the X session with the dGPU still off and checks, for SECONDS,
whether X keeps answering, including the display queries most likely to make
X's NVIDIA driver touch the card. The dGPU is then powered on and the driver
resumed, whatever happened in between. If X stops answering, the screen is
switched to the text console and the reason is printed there.

X may take a long time to notice that the card is not answering, so the
script waits up to PATIENCE seconds for X's first answer before giving up.

Usage: sudo python3 hw_off_in_x.py LOGFILE [SECONDS [PATIENCE]]
"""
import os, subprocess, sys, time

LOG = sys.argv[1]
DWELL = float(sys.argv[2]) if len(sys.argv) > 2 else 60
PATIENCE = float(sys.argv[3]) if len(sys.argv) > 3 else 120
GPU, AUDIO = "0000:01:00.0", "0000:01:00.1"
CFG = f"/sys/bus/pci/devices/{GPU}/config"
CONSOLE = "/dev/tty63"
log = open(LOG, "a")
t0 = time.time()


def say(msg, screen=False):
    log.write("[%6.2f] %s\n" % (time.time() - t0, msg)); log.flush(); os.fsync(log.fileno())
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


def x_env():
    """Display and authority file of the running X server, taken from its command line."""
    pid = sh("pgrep -x Xorg")[1].split("\n")[0]
    args = open(f"/proc/{pid}/cmdline").read().split("\0")
    auth = args[args.index("-auth") + 1] if "-auth" in args else ""
    display = next((a for a in args if a.startswith(":")), ":0")
    return pid, dict(os.environ, DISPLAY=display, XAUTHORITY=auth)


def x_check(label, cmd, timeout=15):
    t = time.time()
    rc, out = sh(cmd, timeout, xenv)
    ok = rc == 0
    say("X check %-22s %s in %.2f s%s" % (label + ":", "ok" if ok else "FAILED (rc %d)" % rc, time.time() - t,
                                           "" if ok else " -- " + out[-200:].replace("\n", " | ")))
    return ok


say("=== dGPU off while the X session is on screen, for %.0f s ===" % DWELL)
xpid, xenv = x_env()
vt = sh("fgconsole")[1]
say("Xorg pid %s on VT %s; gmux 0x%02x; vendor 0x%04x; drivers %s / %s" % (xpid, vt, port(0x750), vendor(), driver(GPU), driver(AUDIO)))
dmesg_mark = sh("dmesg | tail -1")[1]
xlog = next((p for p in ("/var/log/Xorg.0.log", os.path.expanduser("~zerone/.local/share/xorg/Xorg.0.log")) if os.path.exists(p)), None)
xlog_size = os.path.getsize(xlog) if xlog else 0
audio_drv = driver(AUDIO)
suspended = x_alive = False
healthy = True
try:
    x_check("before, core protocol", "xset q")
    say("stop nvidia-persistenced: rc=%d %s" % sh("systemctl stop nvidia-persistenced"))
    say("chvt 63: rc=%d %s" % sh("chvt 63", 10)); time.sleep(1.0)
    say("suspending the NVIDIA driver and cutting dGPU power", screen=True)
    if audio_drv:
        say("unbind audio: %s" % (write_file(f"/sys/bus/pci/devices/{AUDIO}/driver/unbind", AUDIO) or "ok"))
    with open(CFG, "rb") as f:
        saved = f.read(256)
    err = write_file("/proc/driver/nvidia/suspend", "suspend")
    say("nvidia suspend: %s" % (err or "ok"))
    if err:
        raise RuntimeError("driver would not suspend; power not cut")
    suspended = True
    port(0x750, 1); port(0x750, 0); time.sleep(0.3)
    say("gmux power off: port 0x%02x, vendor 0x%04x" % (port(0x750), vendor()))

    # The experiment: back to the desktop with the card dark.
    say("returning to the desktop with the dGPU off", screen=True)
    t = time.time(); rc, out = sh("chvt " + vt, 15)
    say("chvt %s with the dGPU off: rc=%d %s (%.2f s)" % (vt, rc, out, time.time() - t))
    time.sleep(2.0)
    # X may be slow rather than stuck: keep asking, briefly each time, and note when it first answers.
    back_at = time.time()
    healthy = False
    while time.time() - back_at < PATIENCE:
        rc, _ = sh("xset q", 3, xenv)
        if rc == 0:
            healthy = True
            break
    say("X %s with the dGPU off (%.1f s after returning to the desktop)" %
        ("answered" if healthy else "gave no answer", time.time() - back_at))
    end = time.time() + DWELL
    step = 0
    while time.time() < end and healthy:
        step += 1
        healthy = x_check("core protocol", "xset q")
        if healthy and step == 2:
            healthy = x_check("list providers", "xrandr --listproviders")
        if healthy and step == 3:
            healthy = x_check("probe outputs", "xrandr --query", 25)
        if healthy and step == 4:
            healthy = x_check("draw (xdpyinfo)", "xdpyinfo")
        if healthy:
            time.sleep(5)
    x_alive = os.path.exists(f"/proc/{xpid}")
    say("vendor still 0x%04x; Xorg %s; X %s" % (vendor(), "running" if x_alive else "EXITED", "answering" if healthy else "NOT answering"))
    if not healthy:
        sh("chvt 63", 5)
        say("ERROR: the X session stopped answering while the dGPU was off.", screen=True)
        say("The dGPU is being powered back on now.", screen=True)
finally:
    try:
        if suspended:
            if healthy:
                rc, out = sh("chvt 63", 10)
                say("chvt 63 before power-on: rc=%d %s" % (rc, out)); time.sleep(1.0)
                say("powering the dGPU back on", screen=True)
            port(0x750, 1); port(0x750, 3)
            for i in range(50):
                time.sleep(0.1)
                if vendor() != 0xffff:
                    break
            say("gmux power on: port 0x%02x, vendor 0x%04x after %.1f s" % (port(0x750), vendor(), (i + 1) * 0.1))
            time.sleep(0.3)
            fd = os.open(CFG, os.O_WRONLY)
            os.pwrite(fd, saved[0x10:], 0x10); os.pwrite(fd, saved[0x0c:0x10], 0x0c); os.pwrite(fd, saved[0x04:0x06], 0x04)
            os.close(fd)
            err = write_file("/proc/driver/nvidia/suspend", "resume")
            say("nvidia resume: %s" % (err or "ok"))
    except Exception as e:
        say("ERROR during power-on/resume: %r" % e, screen=True)
    if audio_drv:
        say("rebind audio: %s" % (write_file(f"/sys/bus/pci/drivers/{audio_drv}/bind", AUDIO) or "ok"))
    rc, out = sh("chvt " + vt, 15)
    say("chvt %s: rc=%d %s" % (vt, rc, out))
    if not healthy:
        # After a freeze the screen can stay on the console although X is back; going through
        # another console and returning is what brought it back by hand.
        time.sleep(1.5)
        say("bounce through VT 3: rc=%d %s" % sh("chvt 3", 10)); time.sleep(1.5)
        say("chvt %s: rc=%d %s" % ((vt,) + sh("chvt " + vt, 15)))
    say("active VT now: " + sh("fgconsole")[1])
    say("start nvidia-persistenced: rc=%d %s" % sh("systemctl start nvidia-persistenced"))
    time.sleep(2.0)
    back = os.path.exists(f"/proc/{xpid}") and x_check("after, core protocol", "xset q")
    if back:
        x_check("after, probe outputs", "xrandr --query", 25)
    say("after: gmux 0x%02x vendor 0x%04x drivers %s / %s" % (port(0x750), vendor(), driver(GPU), driver(AUDIO)))
    out = sh("dmesg")[1]
    new = out.split(dmesg_mark)[-1] if dmesg_mark in out else out[-4000:]
    say("--- kernel log during the test ---\n" + (new.strip() or "(nothing)"))
    if xlog:
        with open(xlog, errors="replace") as f:
            f.seek(xlog_size)
            say("--- X log during the test ---\n" + (f.read()[-3000:].strip() or "(nothing)"))
    if not back:
        sh("chvt 63", 5)
        say("ERROR: the X session did not come back. The dGPU is powered on again.", screen=True)
        say("Press Ctrl+Alt+F%s to look at it, or Ctrl+Alt+F3 for a login console." % vt, screen=True)
        say("Log: %s" % LOG, screen=True)
    say("=== done: X %s while the dGPU was off, %s afterwards ===" % ("kept answering" if healthy else "STOPPED ANSWERING", "fine" if back else "NOT BACK"))
