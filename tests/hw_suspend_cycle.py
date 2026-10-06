#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Track C, manual: suspend the NVIDIA driver in place, cut dGPU power through gmux, restore.

REAL HARDWARE, ROOT ONLY, Apple classic-gmux laptops only. It switches the
screen to a text console for the duration, as NVIDIA's own sleep script does,
and it can leave the dGPU unusable until a reboot if the driver fails to
resume. Mirrors the sequence in src/daemon/backend.c. Every step is logged
and synced, so the log survives a hang.

Usage: sudo python3 hw_suspend_cycle.py LOGFILE [SECONDS_OFF [SECONDS_BEFORE SECONDS_AFTER]]

SECONDS_BEFORE and SECONDS_AFTER keep the machine on the text console with
the dGPU powered, before and after the cut, so that power draw can be
compared under the same conditions. The phase boundaries are written, as
epoch times, to LOGFILE.marks.
"""
import os, subprocess, sys, time
LOG = sys.argv[1]; OFF_SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 5
PRE = float(sys.argv[3]) if len(sys.argv) > 4 else 0
POST = float(sys.argv[4]) if len(sys.argv) > 4 else 0
marks = open(LOG + ".marks", "w")
def mark(name):
    marks.write("%.3f %s\n" % (time.time(), name)); marks.flush(); os.fsync(marks.fileno())
GPU, AUDIO = "0000:01:00.0", "0000:01:00.1"
CFG = f"/sys/bus/pci/devices/{GPU}/config"
log = open(LOG, "a")
t0 = time.time()
def say(msg):
    log.write("[%6.2f] %s\n" % (time.time() - t0, msg)); log.flush(); os.fsync(log.fileno())
def sh(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()
def port(p, val=None):
    fd = os.open("/dev/port", os.O_RDWR)
    try:
        if val is None: return os.pread(fd, 1, p)[0]
        os.pwrite(fd, bytes([val]), p)
    finally: os.close(fd)
def vendor():
    with open(CFG, "rb") as f: b = f.read(2)
    return b[0] | b[1] << 8
def write_file(path, text):
    try:
        with open(path, "w") as f: f.write(text)
        return None
    except OSError as e: return str(e)
def driver(dev):
    p = f"/sys/bus/pci/devices/{dev}/driver"
    return os.path.basename(os.path.realpath(p)) if os.path.exists(p) else None

say("=== suspend-in-place cycle, off for %.0f s ===" % OFF_SECONDS)
say("kernel %s; gmux power port 0x%02x; vendor 0x%04x; drivers %s / %s" % (os.uname().release, port(0x750), vendor(), driver(GPU), driver(AUDIO)))
vt = sh("fgconsole")[1]
say("active VT: " + vt)
dmesg_mark = sh("dmesg | tail -1")[1]
suspended = powered_off = False
audio_drv = driver(AUDIO)
try:
    say("stop nvidia-persistenced: rc=%d %s" % sh("systemctl stop nvidia-persistenced"))
    # As the vendor's own sleep script does: leave the X session's VT so X stops using the GPU.
    say("chvt 63: rc=%d %s" % sh("chvt 63")); time.sleep(1.0)
    if audio_drv:
        say("unbind audio from %s: %s" % (audio_drv, write_file(f"/sys/bus/pci/devices/{AUDIO}/driver/unbind", AUDIO) or "ok"))
    with open(CFG, "rb") as f: saved = f.read(256)
    say("saved %d bytes of configuration space; command=0x%04x" % (len(saved), saved[4] | saved[5] << 8))
    mark("before_start"); time.sleep(PRE); mark("before_end")
    t = time.time(); err = write_file("/proc/driver/nvidia/suspend", "suspend")
    say("nvidia suspend: %s (%.2f s)" % (err or "ok", time.time() - t))
    if err: raise RuntimeError("driver would not suspend; power not cut")
    suspended = True

    port(0x750, 1); port(0x750, 0)
    for i in range(30):
        time.sleep(0.1)
        if vendor() == 0xffff: break
    say("gmux power off: port 0x%02x, vendor now 0x%04x after %.1f s" % (port(0x750), vendor(), (i + 1) * 0.1))
    powered_off = vendor() == 0xffff
    mark("off_start"); time.sleep(OFF_SECONDS); mark("off_end")
    say("still off after %.0f s: vendor 0x%04x; root port link: %s" % (OFF_SECONDS, vendor(), sh("lspci -vv -s 00:01.0 | grep -o 'LnkSta:.*' | cut -c1-60")[1]))
finally:
    try:
        if suspended:
            port(0x750, 1); port(0x750, 3)
            for i in range(50):
                time.sleep(0.1)
                if vendor() != 0xffff: break
            say("gmux power on: port 0x%02x, vendor 0x%04x after %.1f s" % (port(0x750), vendor(), (i + 1) * 0.1))
            time.sleep(0.3)
            with open(CFG, "rb") as f: cold = f.read(64)
            say("cold card: command=0x%04x bar0=0x%08x" % (cold[4] | cold[5] << 8, int.from_bytes(cold[16:20], "little")))
            # Base addresses and capabilities first, the command register last.
            fd = os.open(CFG, os.O_WRONLY)
            os.pwrite(fd, saved[0x10:], 0x10); os.pwrite(fd, saved[0x0c:0x10], 0x0c); os.pwrite(fd, saved[0x04:0x06], 0x04)
            os.close(fd)
            with open(CFG, "rb") as f: now = f.read(256)
            diff = [hex(i) for i in range(256) if now[i] != saved[i]]
            say("configuration restored; bytes still differing: %s" % (diff[:24] or "none"))
            t = time.time(); err = write_file("/proc/driver/nvidia/suspend", "resume")
            say("nvidia resume: %s (%.2f s)" % (err or "ok", time.time() - t))
            mark("after_start"); time.sleep(POST); mark("after_end")
    except Exception as e:
        say("ERROR during power-on/resume: %r" % e)
    if audio_drv:
        say("rebind audio: %s" % (write_file(f"/sys/bus/pci/drivers/{audio_drv}/bind", AUDIO) or "ok"))
    if vt.isdigit():
        say("chvt %s: rc=%d %s" % ((vt,) + sh("chvt " + vt)))
    say("start nvidia-persistenced: rc=%d %s" % sh("systemctl start nvidia-persistenced"))
    time.sleep(1.0)
    say("after: gmux 0x%02x vendor 0x%04x drivers %s / %s" % (port(0x750), vendor(), driver(GPU), driver(AUDIO)))
    out = sh("dmesg")[1]
    new = out.split(dmesg_mark)[-1] if dmesg_mark in out else out[-4000:]
    say("--- kernel log during the cycle ---\n" + (new.strip() or "(nothing)"))
    say("=== done ===")
