#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Track C: how the display server takes a refusing driver, with the card powered.

REAL HARDWARE, ROOT ONLY, patched NVIDIA driver of revision 5 or later.

Nothing is powered off. With the card on, the driver is frozen in its
refusing way through /proc/driver/nvidia/zss_hold, the display server is
asked for the things that send it into the driver, and the driver is thawed.

The kernel module is made to let go of the card first, and the daemon is
stopped. The first version of this test left both watching. The driver's
resume re-initialises the card, which stops answering for a moment; the
module took that for a loss and froze the driver again halfway through, the
driver gave the card up for good, and the machine stopped (8 October 2026,
docs/track-c/refuse-dry-run.log). The module cannot be made to do the freeze
itself here: it accepts pretended faults only on its test backend.

The daemon is started again at the end and takes the card back under the
module. A second process is started first that thaws the driver and starts
the daemon after 40 s, whatever happens to this one.

Usage: sudo python3 hw_refuse_dry_run.py LOGFILE [vt]
       (DISPLAY and XAUTHORITY of the session in the environment)

  vt   also switch to another virtual terminal and back while refusing: the
       step that hung the display server on a card its driver had given up
"""
import fcntl
import os
import struct
import subprocess
import sys
import time

LOG = open(sys.argv[1], "a")
WITH_VT = len(sys.argv) > 2 and sys.argv[2] == "vt"
HOLD = "/proc/driver/nvidia/zss_hold"
KMOD = "/sys/kernel/zss/0000:01:00.0"
REFUSED = "/sys/module/nvidia/parameters/zss_refused"
t0 = time.time()


def say(text):
    line = f"[{time.time() - t0:7.3f}] {text}"
    print(line, flush=True)
    LOG.write(line + "\n")
    LOG.flush()
    os.fsync(LOG.fileno())


def read(path):
    try:
        return open(path).read().strip()
    except OSError as e:
        return f"? ({e.strerror})"


def hold(word):
    try:
        with open(HOLD, "w") as f:
            f.write(word)
        return "ok"
    except OSError as e:
        return f"refused: {e.strerror}"


def ask(name, cmd, limit=4):
    t = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=limit)
        say(f"  {name}: returned {r.returncode} in {time.time() - t:.2f} s"
            + (f" ({(r.stderr.strip() or r.stdout.strip()).splitlines()[0][:90]})" if r.returncode else ""))
        return True
    except subprocess.TimeoutExpired:
        say(f"  {name}: NO ANSWER in {limit} s")
        return False


def x_state():
    pid = subprocess.run(["pidof", "-s", "Xorg"], capture_output=True, text=True).stdout.strip()
    if not pid:
        return "not running"
    stat = read(f"/proc/{pid}/stat").rsplit(")", 1)[-1].split()
    return f"pid {pid} state {stat[0]} wchan {read(f'/proc/{pid}/wchan')}"


def main():
    if os.geteuid() != 0:
        sys.exit("needs root")
    if not os.path.exists(REFUSED):
        sys.exit("the running NVIDIA driver has no refusal (patch revision 5 is not loaded)")
    say(f"==== refuse dry run at {time.strftime('%Y-%m-%d %H:%M:%S')}; hold={read(HOLD)}; X {x_state()}")
    if read(HOLD) != "running":
        sys.exit("the driver is not running; not touching it")
    # Whatever happens below, the driver is thawed and the daemon started.
    subprocess.Popen(["sh", "-c", f"sleep 40; echo thaw > {HOLD}; sleep 6; systemctl start zssd"],
                     start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ask("before: xdpyinfo", ["xdpyinfo"])
    subprocess.run(["systemctl", "stop", "zssd"])
    if os.path.exists(KMOD):
        try:
            with open("/sys/kernel/zss/unmanage", "w") as f:
                f.write(os.path.basename(KMOD))
        except OSError as e:
            say(f"the kernel module would not let go of the card ({e.strerror}); nothing is tried")
            subprocess.run(["systemctl", "start", "zssd"])
            return
    say(f"daemon stopped; kernel module watching the card: {'yes' if os.path.exists(KMOD) else 'no'}")
    if os.path.exists(KMOD):
        subprocess.run(["systemctl", "start", "zssd"])
        return
    try:
        say(f"freezerefuse: {hold('freezerefuse')}; hold={read(HOLD)}")
        if read(HOLD) != "frozen refusing":
            say("  the driver is not refusing; nothing more is tried")
            return
        time.sleep(0.5)
        say(f"  X {x_state()}; refused so far {read(REFUSED)}")
        alive = ask("xdpyinfo", ["xdpyinfo"])
        alive = ask("xrandr --listproviders", ["xrandr", "--listproviders"]) and alive
        alive = ask("xrandr (asks every output, the NVIDIA ones too)", ["xrandr", "-q"], 8) and alive
        alive = ask("nvidia-smi (an ordinary program calling the driver)", ["nvidia-smi", "-L"]) and alive
        say(f"  X {x_state()}; refused so far {read(REFUSED)}")
        if WITH_VT and alive:
            ctl = os.open("/dev/tty0", os.O_RDWR)
            origin = struct.unpack("HHH", fcntl.ioctl(ctl, 0x5603, b"\0" * 6))[0]
            say(f"  switching from virtual terminal {origin} to 63 and back")
            subprocess.run(["timeout", "6", "chvt", "63"])
            time.sleep(1.5)
            say(f"  active terminal: {read('/sys/class/tty/tty0/active')}; X {x_state()}")
            subprocess.run(["timeout", "6", "chvt", str(origin)])
            time.sleep(1.5)
            say(f"  active terminal: {read('/sys/class/tty/tty0/active')}; X {x_state()}")
            ask("xdpyinfo after the switch", ["xdpyinfo"])
        elif WITH_VT:
            say("  the terminal switch was skipped: the display server had already stopped answering")
    finally:
        if read(HOLD) != "running":
            say(f"thaw: {hold('thaw')}; hold={read(HOLD)}")
        time.sleep(1)
        ask("after: xdpyinfo", ["xdpyinfo"], 8)
        time.sleep(5)  # let the driver finish with the card before anything watches it again
        ask("after: nvidia-smi", ["nvidia-smi", "-L"], 8)
        say(f"  hold={read(HOLD)}; X {x_state()}; callers refused in all: {read(REFUSED)}")
        subprocess.run(["systemctl", "start", "zssd"])
        time.sleep(1)
        say(f"  daemon: {subprocess.run(['systemctl', 'is-active', 'zssd'], capture_output=True, text=True).stdout.strip()}; "
            f"kernel module watching the card again: {'yes' if os.path.exists(KMOD) else 'no'}")


if __name__ == "__main__":
    main()
