#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Track C: power cut under a running driver, on the reference laptop.

REAL HARDWARE, ROOT ONLY, Apple classic-gmux laptops only. THIS CAN HANG OR
CRASH THE MACHINE. Save everything first, and expect to reboot afterwards:
a driver whose card lost power while it was running usually stays unusable
until it is reloaded.

Nothing is suspended and nobody is told. The gmux power rail of the discrete
GPU is switched off while an application renders on it, and the script then
only watches: does the kernel module notice, does the daemon move the
application, does the application live, does the display server answer.

Every observation is written and synced at once, so the log survives a crash.

Usage: sudo python3 hw_surprise_cut.py check|cut|restore|diagnose LOGFILE APP_PID
       (DISPLAY and XAUTHORITY of the session in the environment)

  check    look, and say whether everything is in place; change nothing
  cut      switch the power rail off and observe for 20 s; nothing is restored
  restore  switch the rail on again and ask the kernel module to take the device back
  diagnose cut the rail, record where the X server and the application wait
           (state, wait channel and kernel stack of every thread), then switch
           the rail back on after 4 s and ask the module to take the device
           back, recording again: so that a desktop stopped by the loss comes
           back by itself, and the log says what stopped it
"""
import glob
import os
import subprocess
import sys
import time

MODE, LOG, APP = sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0
GPU = "0000:01:00.0"
KMOD = f"/sys/kernel/zss/{GPU}"
CFG = f"/sys/bus/pci/devices/{GPU}/config"
GMUX_POWER = 0x50
log = open(LOG, "a")
t0 = time.time()


def say(text):
    line = f"[{time.time() - t0:7.3f}] {text}"
    print(line, flush=True)
    log.write(line + "\n")
    log.flush()
    os.fsync(log.fileno())


def read(path, default="?"):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError as e:
        return f"{default} ({e.strerror})"


def bus():
    try:
        with open(CFG, "rb") as f:
            return f.read(2).hex()
    except OSError:
        return "unreadable"


def gmux_base():
    for dev in glob.glob("/sys/bus/pnp/devices/*"):
        if read(dev + "/id", "").upper().startswith("APP000B"):
            for line in read(dev + "/resources", "").split("\n"):
                if line.startswith("io "):
                    return int(line.split()[1].split("-")[0], 16)
    return None


def port(addr, value=None):
    fd = os.open("/dev/port", os.O_RDWR)
    try:
        os.lseek(fd, addr, 0)
        if value is None:
            return os.read(fd, 1)[0]
        os.write(fd, bytes([value]))
    finally:
        os.close(fd)


def daemon_status():
    try:
        r = subprocess.run(["zssctl", "status", GPU], capture_output=True, text=True, timeout=3)
        return " | ".join(l.strip() for l in r.stdout.strip().split("\n")) or r.stderr.strip()
    except subprocess.TimeoutExpired:
        return "NO ANSWER in 3 s"


def app_state():
    if not os.path.exists(f"/proc/{APP}"):
        return "GONE"
    stat = read(f"/proc/{APP}/stat").rsplit(")", 1)[-1].split()
    held = set()
    for fd in glob.glob(f"/proc/{APP}/fd/*"):
        try:
            target = os.readlink(fd)
        except OSError:
            continue
        if target.startswith("/dev/nvidia") or target.startswith("/dev/dri/"):
            held.add(target)
    return f"alive, state {stat[0]}, holds {sorted(held) or 'no GPU node'}"


def x_answers():
    try:
        r = subprocess.run(["xdpyinfo"], capture_output=True, timeout=2)
        return "answers" if r.returncode == 0 else f"error {r.returncode}"
    except subprocess.TimeoutExpired:
        return "NO ANSWER in 2 s"
    except OSError as e:
        return f"cannot ask ({e.strerror})"


def waits(pid, who):
    """Where each thread of a process is: its state, what it waits on, and its kernel stack."""
    if not pid or not os.path.exists(f"/proc/{pid}"):
        say(f"  {who}: not running")
        return
    for task in sorted(glob.glob(f"/proc/{pid}/task/*"), key=lambda p: int(p.rsplit("/", 1)[1])):
        stat = read(task + "/stat").rsplit(")", 1)[-1].split()
        name = read(task + "/comm")
        stack = " < ".join(l.split(" ", 1)[-1].split("+")[0] for l in read(task + "/stack").split("\n")[:8] if l)
        if stat[0] != "S" or "nv" in stack or "zss" in stack or "down" in stack:
            say(f"  {who} thread {task.rsplit('/', 1)[1]} ({name}) state {stat[0]} wchan {read(task + '/wchan')}: {stack}")
    say(f"  {who}: {len(glob.glob(f'/proc/{pid}/task/*'))} thread(s); those not shown sleep in user space waits")


def stuck():
    """Every thread on the machine in uninterruptible sleep."""
    found = 0
    for task in glob.glob("/proc/[0-9]*/task/[0-9]*"):
        stat = read(task + "/stat").rsplit(")", 1)[-1].split()
        if stat and stat[0] == "D":
            found += 1
            stack = " < ".join(l.split(" ", 1)[-1].split("+")[0] for l in read(task + "/stack").split("\n")[:6] if l)
            say(f"  uninterruptible: {task} ({read(task + '/comm')}): {stack}")
    if not found:
        say("  no thread on the machine is in uninterruptible sleep")


class Kmsg:
    def __init__(self):
        self.fd = os.open("/dev/kmsg", os.O_RDONLY | os.O_NONBLOCK)
        os.lseek(self.fd, 0, os.SEEK_END)

    def new(self):
        out = []
        while True:
            try:
                line = os.read(self.fd, 8192).decode(errors="replace")
            except BlockingIOError:
                break
            except OSError:
                continue
            text = line.split(";", 1)[-1].split("\n")[0]
            out.append(text)
        return out


def look(kmsg, tag):
    say(f"{tag}: bus={bus()} module={read(KMOD + '/state')} driver_frozen={read(KMOD + '/driver_frozen')} "
        f"nvidia={read('/proc/driver/nvidia/zss_hold')} [{read(KMOD + '/functions').replace(chr(10), '; ')}]")
    say(f"{tag}: daemon: {daemon_status()}")
    say(f"{tag}: application: {app_state()}")
    say(f"{tag}: X server: {x_answers()}")
    for line in kmsg.new():
        say(f"{tag}: kernel: {line[:220]}")


def main():
    if os.geteuid() != 0:
        sys.exit("needs root")
    base = gmux_base()
    kmsg = Kmsg()
    say(f"==== {MODE} at {time.strftime('%Y-%m-%d %H:%M:%S')}, application pid {APP}, gmux base {hex(base) if base else None}")
    if base is None:
        sys.exit("no classic gmux found")

    if MODE == "check":
        look(kmsg, "check")
        mine = [part for part in daemon_status().split(" | ") if part.startswith(f"pid {APP} ")]
        ok = (bus() != "ffff" and read(KMOD + "/state") == "on" and read(KMOD + "/backend") == "gmux"
              and "/dev/nvidia0" in app_state() and bool(mine) and " migratable" in mine[0])
        say("READY: everything is in place" if ok else "NOT READY: see the lines above")
        return 0 if ok else 1

    if MODE == "cut":
        look(kmsg, "before")
        say("CUTTING the discrete GPU's power rail now, with its driver running and nothing warned")
        t = time.time()
        port(base + GMUX_POWER, 1)
        port(base + GMUX_POWER, 0)
        seen_lost = seen_moved = None
        for i in range(80):
            time.sleep(0.05 if i < 20 else 0.25)
            now = time.time() - t
            if seen_lost is None and read(KMOD + "/state") == "lost":
                seen_lost = now
                say(f"the kernel module reported the loss {now * 1000:.0f} ms after the cut")
            if seen_moved is None and i % 4 == 3 and "migrated-away" in daemon_status():
                seen_moved = now
                say(f"the daemon reported the application moved away {now:.2f} s after the cut")
            if i in (3, 9, 19, 27, 39, 59, 79):
                look(kmsg, f"+{now:5.2f}s")
        say("observation over; the rail is still OFF and nothing was restored")
        say(f"summary: loss seen by the module after {seen_lost}, application moved after {seen_moved}, "
            f"application {app_state()}, X {x_answers()}")
        return 0

    if MODE == "diagnose":
        xorg = int(subprocess.run(["pidof", "-s", "Xorg"], capture_output=True, text=True).stdout.strip() or 0)
        look(kmsg, "before")
        say(f"DIAGNOSE: cutting the rail; X server pid {xorg}; the rail comes back on by itself after 4 s")
        t = time.time()
        port(base + GMUX_POWER, 1)
        port(base + GMUX_POWER, 0)
        for at in (0.5, 2.0):
            time.sleep(max(0, at - (time.time() - t)))
            say(f"+{time.time() - t:5.2f}s: module={read(KMOD + '/state')} driver_frozen={read(KMOD + '/driver_frozen')}")
            waits(xorg, "X server")
            waits(APP, "application")
            stuck()
        time.sleep(max(0, 4.0 - (time.time() - t)))
        say("switching the power rail back on")
        port(base + GMUX_POWER, 1)
        port(base + GMUX_POWER, 3)
        for _ in range(30):
            time.sleep(0.1)
            if bus() != "ffff":
                break
        if read(KMOD + "/driver_frozen") == "1" and APP and os.path.exists(f"/proc/{APP}"):
            # Resuming a frozen driver under a program that was drawing on the card took the machine down twice.
            say("the driver is frozen and the application still exists: NOT asking the module to take the device back")
        else:
            say(f"bus now reads {bus()}; asking the kernel module to take the device back")
            try:
                with open(KMOD + "/power", "w") as f:
                    f.write("on")
                say("the module accepted")
            except OSError as e:
                say(f"the module refused: {e.strerror}: {read(KMOD + '/last_error')}")
        for at in (6.0, 10.0):
            time.sleep(max(0, at - (time.time() - t)))
            look(kmsg, f"+{time.time() - t:5.2f}s")
            waits(xorg, "X server")
        return 0

    if MODE == "restore":
        look(kmsg, "before")
        say("switching the power rail back on")
        port(base + GMUX_POWER, 1)
        port(base + GMUX_POWER, 3)
        for _ in range(30):
            time.sleep(0.1)
            if bus() != "ffff":
                break
        say(f"bus now reads {bus()}; asking the kernel module to take the device back")
        try:
            with open(KMOD + "/power", "w") as f:
                f.write("on")
            say("the module accepted")
        except OSError as e:
            say(f"the module refused: {e.strerror}: {read(KMOD + '/last_error')}")
        time.sleep(1)
        look(kmsg, "after")
        return 0
    sys.exit("unknown mode")


if __name__ == "__main__":
    sys.exit(main())
