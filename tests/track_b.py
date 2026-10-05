#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Track B: the full detach and attach sequence, in a QEMU guest.

The guest boots the host's kernel with a tiny initramfs (tests/qemu/init.c)
and uses the host's root file system read-only, so no image is downloaded.
A `bochs-display` card sits on a hot-pluggable PCIe root port. Inside the
guest, tests/track_b_guest.py runs zssd as root with the pciehp-slot backend;
when it needs the card physically removed or returned it asks this script,
which does so through QMP.

Usage: track_b.py [--keep] [--verbose]
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.abspath(os.environ.get("ZSS_BUILD") or os.path.join(HERE, "..", "build"))
KERNEL_RELEASE = os.uname().release
MODULES = ["fs/netfs/netfs", "net/9p/9pnet", "net/9p/9pnet_virtio", "fs/9p/9p", "drivers/gpu/drm/tiny/bochs"]
GPU_ID, PORT_ID = "gpu1", "rp1"
TIMEOUT = 420


def build_initramfs(work):
    root = os.path.join(work, "initramfs")
    os.makedirs(os.path.join(root, "modules"))
    subprocess.run(["gcc", "-static", "-O1", "-o", os.path.join(root, "init"),
                    os.path.join(HERE, "qemu", "init.c")], check=True)
    order = []
    base = f"/usr/lib/modules/{KERNEL_RELEASE}/kernel"
    for mod in MODULES:
        name = os.path.basename(mod) + ".ko"
        for suffix, cmd in ((".ko.zst", ["zstd", "-dqc"]), (".ko.xz", ["xz", "-dc"]), (".ko.gz", ["gzip", "-dc"]),
                            (".ko", ["cat"])):
            src = f"{base}/{mod}{suffix}"
            if os.path.exists(src):
                with open(os.path.join(root, "modules", name), "wb") as out:
                    subprocess.run(cmd + [src], stdout=out, check=True)
                order.append(name)
                break
        # A module that is absent is assumed to be built into the kernel.
    with open(os.path.join(root, "modules", "order"), "w") as f:
        f.write("\n".join(order) + "\n")
    image = os.path.join(work, "initramfs.cpio")
    names = subprocess.run(["find", ".", "-print0"], cwd=root, capture_output=True, check=True).stdout
    with open(image, "wb") as out:
        subprocess.run(["cpio", "--null", "-o", "-H", "newc", "--quiet", "-R", "+0:+0"], cwd=root, input=names,
                       stdout=out, check=True)
    return image


class Qmp:
    def __init__(self, path):
        deadline = time.time() + 20
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.file = self.sock.makefile("rw")
        self.events = []
        json.loads(self.file.readline())  # greeting
        self.command("qmp_capabilities")

    def _read(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError("QEMU closed the QMP connection")
        return json.loads(line)

    def command(self, name, **arguments):
        msg = {"execute": name}
        if arguments:
            msg["arguments"] = arguments
        self.file.write(json.dumps(msg) + "\n")
        self.file.flush()
        while True:
            reply = self._read()
            if "event" in reply:
                self.events.append(reply)
                continue
            return reply

    def wait_event(self, name, timeout=30):
        self.sock.settimeout(timeout)
        try:
            while True:
                for e in self.events:
                    if e["event"] == name:
                        self.events.remove(e)
                        return e
                reply = self._read()
                if "event" in reply:
                    self.events.append(reply)
        except (socket.timeout, TimeoutError):
            return None
        finally:
            self.sock.settimeout(None)


def serve(qmp, work, log):
    """Carries out the guest's requests until it writes its exit code."""
    done = set()
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        if os.path.exists(os.path.join(work, "exit-code")):
            return True
        for name in sorted(os.listdir(work)):
            if not name.startswith("req-") or name in done:
                continue
            done.add(name)
            with open(os.path.join(work, name)) as f:
                action = f.read().strip()
            result = "ok"
            if action == "unplug":
                reply = qmp.command("device_del", id=GPU_ID)
                if reply.get("error", {}).get("class") == "DeviceNotFound":
                    # QEMU drops the card by itself once the guest has powered the slot off.
                    result = "gone"
                else:
                    event = qmp.wait_event("DEVICE_DELETED", 30) if "return" in reply else None
                    result = "ok" if event else f"failed: {reply}"
            elif action == "plug":
                reply = qmp.command("device_add", driver="bochs-display", id=GPU_ID, bus=PORT_ID)
                result = "ok" if "return" in reply else f"failed: {reply}"
            else:
                result = f"failed: unknown action {action}"
            log(f"host: {action} -> {result}")
            with open(os.path.join(work, name.replace("req-", "ack-")), "w") as f:
                f.write(result + "\n")
        time.sleep(0.05)
    return False


def main():
    keep = "--keep" in sys.argv
    verbose = "--verbose" in sys.argv
    qemu = shutil.which("qemu-system-x86_64")
    if not qemu or not os.access("/dev/kvm", os.R_OK | os.W_OK):
        print("SKIP  track B (needs qemu-system-x86_64 and access to /dev/kvm)")
        return 77
    kernel = f"/usr/lib/modules/{KERNEL_RELEASE}/vmlinuz"
    if not os.path.exists(kernel):
        print(f"SKIP  track B (no kernel image at {kernel})")
        return 77

    work = tempfile.mkdtemp(prefix="zss-track-b-")
    host_log = []

    def log(text):
        host_log.append(text)
        if verbose:
            print(text, flush=True)

    try:
        image = build_initramfs(work)
        script = os.path.join(HERE, "track_b_guest.py")
        if os.environ.get("ZSS_GUEST_SCRIPT"):
            # The guest has its own /tmp, so an ad-hoc script travels in the work share.
            shutil.copy(os.environ["ZSS_GUEST_SCRIPT"], os.path.join(work, "guest_override.py"))
            script = "/mnt/guest_override.py"
        with open(os.path.join(work, "run.sh"), "w") as f:
            f.write(f"export ZSS_BUILD='{BUILD}'\nexec python3 '{script}'\n")
        serial = os.path.join(work, "serial.log")
        qmp_path = os.path.join(work, "qmp")
        proc = subprocess.Popen([
            qemu, "-machine", "q35,accel=kvm", "-cpu", "host", "-m", "2048", "-smp", "2",
            "-nodefaults", "-display", "none", "-no-reboot",
            # Native PCIe hot-plug (pciehp). QEMU's default, ACPI hot-plug, ejects the
            # device as soon as the guest powers the slot off, which a real slot does not do.
            "-global", "ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off",
            "-kernel", kernel, "-initrd", image,
            "-append", "console=ttyS0 panic=-1 loglevel=4",
            "-serial", f"file:{serial}",
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
            "-device", f"pcie-root-port,id={PORT_ID},chassis=1,slot=1",
            "-device", f"bochs-display,id={GPU_ID},bus={PORT_ID}",
            "-fsdev", "local,id=root,path=/,security_model=none,readonly=on",
            "-device", "virtio-9p-pci,fsdev=root,mount_tag=hostroot",
            "-fsdev", f"local,id=work,path={work},security_model=none",
            "-device", "virtio-9p-pci,fsdev=work,mount_tag=work",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            finished = serve(Qmp(qmp_path), work, log)
        except Exception as e:  # QEMU died, or QMP never came up
            finished = False
            log(f"host: {e}")
        try:
            proc.wait(15)
        except subprocess.TimeoutExpired:
            proc.kill()

        results = ""
        if os.path.exists(os.path.join(work, "results.txt")):
            with open(os.path.join(work, "results.txt")) as f:
                results = f.read()
        print(results, end="")
        code = 1
        if finished:
            with open(os.path.join(work, "exit-code")) as f:
                code = int(f.read().strip() or 1)
        if code != 0 or verbose:
            if not finished:
                print("FAIL  the guest did not finish")
            err = proc.stderr.read().decode(errors="replace")
            if err.strip():
                print("--- qemu:\n" + err[-1500:])
            if os.path.exists(serial):
                with open(serial, errors="replace") as f:
                    print("--- guest console (tail):\n" + f.read()[-6000:])
            print("--- host actions:\n" + "\n".join(host_log))
        return code
    finally:
        if keep:
            print(f"work directory kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
