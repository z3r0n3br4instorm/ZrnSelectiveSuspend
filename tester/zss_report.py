#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""zss-report: what a machine has for ZrnSelectiveSuspend, for testers to send back.

It collects facts about the GPUs, their drivers, the power and IOMMU hardware
around them, and what Vulkan and OpenGL see, and runs checks that only read.
It also runs the project's moving tests: programs moved between this
machine's GPUs and back by a test daemon that switches nothing off (they need
the ZSS build that comes with the kit; a few windows appear for a moment).
Nothing is installed or changed, and nothing is sent: the report is written to
a folder and an archive, the tester reads it, and sends it with their own mail
program to the address below.

Before anything is written, the machine's name, the user's name and home
folder, disk and partition identifiers, serial numbers and network addresses
are replaced by placeholders.

Usage:
  zss-report                 collect, write the report, print where it is
  zss-report --mail          the same, then open the mail program with the address filled in
  zss-report --out DIR       write the report folder under DIR (default: the home folder)
  zss-report --no-tests      facts only, without the moving tests
  zss-report --debug-log F   write the whole report as one text file F (with --out, as well as the archive)
  zss-report --include N=F   add file F to the report under the name N
  zss-report --quiet         print only the archive's path
  zss-report --full-test     after the report, install ZSS from this source tree and run its
                             tests and one real power-off and power-on (asks first; needs sudo)

The Qt window, zss-report-gui, does the same with buttons.
"""
import datetime
import glob
import json
import os
import platform
import pwd
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import urllib.parse

REPORT_TO = "omethabeyrathne3@gmail.com"
VERSION = "1"
PCI = "/sys/bus/pci/devices"

# ---- redaction ----------------------------------------------------------------------------


class Redactor:
    """Replaces what identifies the machine or the person in everything the report keeps."""

    def __init__(self):
        self.pairs = []
        host = socket.gethostname()
        user = pwd.getpwuid(os.getuid()).pw_name
        home = os.path.expanduser("~")
        if home and home != "/":
            self.pairs.append((home, "<home>"))
        if host:
            self.pairs.append((host, "<host>"))
        if user and len(user) > 2:
            self.pairs.append((user, "<user>"))
        # Run through sudo (the installer): the person is the one who ran it.
        su = os.environ.get("SUDO_USER")
        if su and su != user:
            try:
                sh = pwd.getpwnam(su).pw_dir
                if sh and sh != "/":
                    self.pairs.insert(0, (sh, "<home>"))
            except KeyError:
                pass
            if len(su) > 2:
                self.pairs.append((su, "<user>"))
        self.patterns = [
            (re.compile(r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b"), "<uuid>"),
            (re.compile(r"\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b"), "<mac>"),
            (re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b"), "<ip>"),
            (re.compile(r"(?i)(serial[^:=\n]*[:=]\s*)\S+"), r"\1<serial>"),
            (re.compile(r"(?i)(serial number\s+)[0-9a-f-]+"), r"\1<serial>"),  # lspci: "Device Serial Number 00-11-..."
            (re.compile(r"[\w.+-]+@[\w-]+\.[\w.]+"), "<email>"),
        ]

    def __call__(self, text):
        if not isinstance(text, str):
            return text
        for a, b in self.pairs:
            text = text.replace(a, b)
        for pat, rep in self.patterns:
            text = pat.sub(rep, text)
        return text


# ---- small helpers --------------------------------------------------------------------------


def read(path, default=""):
    try:
        with open(path, errors="replace") as f:
            return f.read().strip()
    except OSError:
        return default


def run(cmd, timeout=20, env=None):
    """Runs a command; returns (exit status or None if it is not there, output)."""
    if not shutil.which(cmd[0]):
        return None, f"{cmd[0]}: not installed"
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        return r.returncode, (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        return -1, f"{' '.join(cmd)}: timed out after {timeout} s"
    except OSError as e:
        return -1, str(e)


def link_name(path):
    try:
        return os.path.basename(os.readlink(path))
    except OSError:
        return ""


# ---- the checks -----------------------------------------------------------------------------
#
# Each check returns (status, one-line summary, details dict). Status is "ok", "warn",
# "fail" or "info". A check never changes anything.


def check_system(raw):
    osr = dict(re.findall(r'^(\w+)="?([^"\n]*)"?$', read("/etc/os-release"), re.M))
    dmi = {k: read(f"/sys/class/dmi/id/{k}") for k in ("sys_vendor", "product_name", "product_version",
                                                         "board_vendor", "board_name", "bios_vendor",
                                                         "bios_version", "bios_date")}
    cpu = re.search(r"^model name\s*:\s*(.*)$", read("/proc/cpuinfo"), re.M)
    mem = re.search(r"^MemTotal:\s*(\d+)", read("/proc/meminfo"), re.M)
    d = {
        "distribution": osr.get("PRETTY_NAME", "unknown"),
        "kernel": platform.release(),
        "architecture": platform.machine(),
        "machine": dmi,
        "cpu": cpu.group(1) if cpu else "unknown",
        "memory_mb": int(mem.group(1)) // 1024 if mem else None,
        "firmware": "UEFI" if os.path.isdir("/sys/firmware/efi") else "BIOS",
        "secure_boot": read("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c")[-1:] == "\x01",
        "session": os.environ.get("XDG_SESSION_TYPE", "unknown"),
        "desktop": os.environ.get("XDG_CURRENT_DESKTOP", os.environ.get("DESKTOP_SESSION", "unknown")),
        "kernel_command_line": read("/proc/cmdline"),
    }
    raw["os-release.txt"] = read("/etc/os-release")
    raw["cmdline.txt"] = d["kernel_command_line"]
    return "info", f"{dmi['sys_vendor']} {dmi['product_name']}, {d['distribution']}, kernel {d['kernel']}", d


def gpus():
    out = []
    for dev in sorted(glob.glob(f"{PCI}/*")):
        if read(f"{dev}/class").startswith("0x03"):
            out.append(os.path.basename(dev))
    return out


def check_gpus(raw):
    rc, lspci = run(["lspci", "-nnk"])
    raw["lspci-nnk.txt"] = lspci
    rc, lspciv = run(["lspci", "-vv"], timeout=30)
    raw["lspci-vv.txt"] = lspciv
    found = []
    for addr in gpus():
        dev = f"{PCI}/{addr}"
        stem = addr.rsplit(".", 1)[0]
        functions = sorted(os.path.basename(f) for f in glob.glob(f"{PCI}/{stem}.*"))
        group = link_name(f"{dev}/iommu_group")
        members = sorted(os.listdir(f"/sys/kernel/iommu_groups/{group}/devices")) if group else []
        fw = os.path.realpath(f"{dev}/firmware_node") if os.path.exists(f"{dev}/firmware_node") else ""
        port = os.path.dirname(os.path.realpath(dev))
        g = {
            "address": addr,
            "ids": f"{read(dev + '/vendor')[2:]}:{read(dev + '/device')[2:]}",
            "subsystem": f"{read(dev + '/subsystem_vendor')[2:]}:{read(dev + '/subsystem_device')[2:]}",
            "name": next((l.split(": ", 1)[-1] for l in lspci.splitlines() if l.startswith(addr[5:])), ""),
            "driver": link_name(f"{dev}/driver"),
            "functions": {f: link_name(f"{PCI}/{f}/driver") for f in functions},
            "boot_vga": read(f"{dev}/boot_vga"),
            "power_state": read(f"{dev}/power_state"),
            "runtime_status": read(f"{dev}/power/runtime_status"),
            "runtime_control": read(f"{dev}/power/control"),
            "d3cold_allowed": read(f"{dev}/d3cold_allowed"),
            "link": f"{read(dev + '/current_link_speed')} x{read(dev + '/current_link_width')}",
            "iommu_group": group,
            "iommu_group_members": members,
            "acpi_node": bool(fw),
            "acpi_power_resources_d3": os.path.isdir(f"{dev}/firmware_node/power_resources_D3hot")
                                       or os.path.isdir(f"{port}/firmware_node/power_resources_D3hot"),
            "hotplug_slot": os.path.exists(f"{dev}/slot") or bool(glob.glob(f"{port}/*:pcie*04")),
            "drm": sorted(os.listdir(f"{dev}/drm")) if os.path.isdir(f"{dev}/drm") else [],
        }
        found.append(g)
    discrete = [g for g in found if g["boot_vga"] != "1"]
    summary = ", ".join(f"{g['name'] or g['ids']} ({g['driver'] or 'no driver'})" for g in found) or "none"
    status = "ok" if discrete else "warn"
    return status, f"{len(found)} GPU(s): {summary}", {"gpus": found}


def check_power(raw, gpu_info):
    """Which way this machine could cut power to a GPU, as ZSS_Interceptor's backends see it."""
    ways = []
    gmux = os.path.isdir("/sys/bus/acpi/devices/APP000B:00") or "gmux" in read("/proc/ioports")
    if gmux:
        ways.append("apple-gmux")
    for g in gpu_info["gpus"]:
        if g["boot_vga"] == "1":
            continue
        if g["acpi_power_resources_d3"]:
            ways.append(f"acpi ({g['address']})")
        if g["hotplug_slot"]:
            ways.append(f"pciehp-slot ({g['address']})")
        if g["runtime_status"]:
            ways.append(f"runtime-pm ({g['address']}: {g['runtime_status']}, control={g['runtime_control']})")
    raw["ioports.txt"] = read("/proc/ioports")
    hard = [w for w in ways if not w.startswith("runtime-pm")]
    status = "ok" if hard else "warn"
    return status, "power can be cut through: " + (", ".join(hard) if hard else "nothing ZSS has a backend for"), {"ways": ways}


def check_drivers(raw):
    rc, mods = run(["lsmod"])
    keep = re.compile(r"^(nvidia\S*|nouveau|amdgpu|radeon|i915|xe|apple_gmux|vfio\S*|zss|snd_hda_intel|thunderbolt|kvm\S*)\s",
                      re.M)
    lines = [l for l in mods.splitlines() if keep.match(l)]
    raw["lsmod-gpu.txt"] = "\n".join(lines)
    nv = read("/proc/driver/nvidia/version")
    d = {"modules": [l.split()[0] for l in lines], "nvidia": nv.splitlines()[0] if nv else ""}
    for p in glob.glob("/proc/driver/nvidia/gpus/*/power"):
        raw[f"nvidia-power-{p.split('/')[-2]}.txt"] = read(p)
    d["nvidia_zss_patch"] = os.path.exists("/sys/module/nvidia/parameters/zss_wake_requests")
    for mod in ("nvidia", "nouveau", "amdgpu", "i915", "xe"):
        rc, info = run(["modinfo", "-F", "version", mod])
        if rc == 0 and info:
            d[f"{mod}_version"] = info.splitlines()[0]
    return "info", "drivers: " + (", ".join(d["modules"]) or "none of interest loaded"), d


def check_iommu(raw):
    groups = os.listdir("/sys/kernel/iommu_groups") if os.path.isdir("/sys/kernel/iommu_groups") else []
    dmar = os.path.exists("/sys/firmware/acpi/tables/DMAR")
    ivrs = os.path.exists("/sys/firmware/acpi/tables/IVRS")
    rc, klog = run(["journalctl", "-k", "-b", "--no-pager", "-q"], timeout=30)
    if rc != 0:
        rc, klog = run(["dmesg"])
    readable = rc == 0
    irq = [l for l in klog.splitlines() if re.search(r"DMAR-IR|AMD-Vi|interrupt remapping|IOMMU", l)] if readable else []
    raw["kernel-iommu.txt"] = "\n".join(irq) if readable else "kernel log not readable without more rights"
    remap = None
    if irq:
        remap = not any(re.search(r"Failed to enable irq remapping|Not enabling interrupt remapping", l) for l in irq)
    d = {"enabled": bool(groups), "groups": len(groups), "firmware_table": "DMAR" if dmar else "IVRS" if ivrs else "",
         "interrupt_remapping": remap, "kernel_log_readable": readable}
    if groups:
        return "ok", f"IOMMU on ({len(groups)} groups), interrupt remapping " + \
               ("yes" if remap else "no" if remap is False else "unknown"), d
    return "warn", "IOMMU off" + (" (the firmware offers one)" if dmar or ivrs else " (no firmware table)"), d


def check_kernel_log(raw):
    rc, klog = run(["journalctl", "-k", "-b", "--no-pager", "-q"], timeout=30)
    if rc != 0:
        rc, klog = run(["dmesg"])
    if rc != 0:
        return "info", "kernel log not readable (run with more rights to include it)", {}
    keep = re.compile(r"(?i)nvidia|NVRM|nouveau|amdgpu|i915|\bxe\b|drm|gmux|vga_switcheroo|pcieport|AER|vfio|zss:|Xid|"
                      r"thunderbolt|D3cold|fallen off the bus")
    lines = [l for l in klog.splitlines() if keep.search(l)]
    raw["kernel-gpu.txt"] = "\n".join(lines[-2000:])
    bad = [l for l in lines if re.search(r"Xid|fallen off|BUG|Oops|failed|timeout", l, re.I)]
    return ("warn" if bad else "ok"), f"{len(lines)} GPU-related kernel messages, {len(bad)} look like errors", \
           {"errors": bad[-30:]}


def check_vulkan(raw):
    icds = sorted(glob.glob("/usr/share/vulkan/icd.d/*.json") + glob.glob("/etc/vulkan/icd.d/*.json") +
                  glob.glob("/usr/local/share/vulkan/icd.d/*.json"))
    per = {}
    for icd in icds:
        env = dict(os.environ, VK_DRIVER_FILES=icd, VK_ICD_FILENAMES=icd)
        env.pop("ZSS_START_ON", None)
        rc, out = run(["vulkaninfo", "--summary"], timeout=30, env=env)
        name = os.path.basename(icd)
        raw[f"vulkaninfo-{name}.txt"] = out
        if rc is None:
            return "warn", "vulkaninfo is not installed (package vulkan-tools)", {"icds": icds}
        devs = re.findall(r"deviceName\s*=\s*(.*)", out)
        apis = re.findall(r"apiVersion\s*=\s*(\S+)", out)
        drivers = re.findall(r"driverInfo\s*=\s*(.*)", out)
        if devs:
            per[name] = [{"device": d, "api": a, "driver": v} for d, a, v in zip(devs, apis, drivers)]
    total = sum(len(v) for v in per.values())
    return ("ok" if total else "fail"), f"Vulkan: {total} device(s) through {len(per)} of {len(icds)} drivers", \
           {"icds": icds, "devices": per}


def check_opengl(raw):
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        return "info", "OpenGL: no display to ask", {}
    rc, out = run(["glxinfo", "-B"])
    raw["glxinfo-B.txt"] = out
    if rc is None:
        return "warn", "glxinfo is not installed (package mesa-utils)", {}
    r = re.search(r"OpenGL renderer string:\s*(.*)", out)
    v = re.search(r"OpenGL version string:\s*(.*)", out)
    return ("ok" if r else "warn"), f"OpenGL: {r.group(1) if r else '?'} ({v.group(1) if v else '?'})", {}


def check_display(raw):
    log = ""
    for p in ("/var/log/Xorg.0.log", os.path.expanduser("~/.local/share/xorg/Xorg.0.log")):
        if os.path.exists(p):
            log = read(p)
            break
    keep = [l for l in log.splitlines() if re.search(r"(?i)\(II\) (modeset|NVIDIA|AMDGPU|intel)|Adding drm device|"
                                                     r"OutputClass|ServerLayout|AutoAddGPU|\(EE\)", l)]
    raw["xorg-gpu.txt"] = "\n".join(keep[:400])
    conf = {p: read(p) for p in glob.glob("/etc/X11/xorg.conf.d/*.conf") + ["/etc/X11/xorg.conf"] if os.path.exists(p)}
    raw["xorg-conf.txt"] = "\n\n".join(f"# {k}\n{v}" for k, v in conf.items())
    rc, prov = run(["xrandr", "--listproviders"]) if os.environ.get("DISPLAY") else (None, "")
    raw["xrandr-providers.txt"] = prov
    seats = run(["loginctl", "list-seats", "--no-legend"])[1]
    return "info", f"display server: {os.environ.get('XDG_SESSION_TYPE', 'unknown')}; seats: {' '.join(seats.split()) or '?'}", \
           {"x_errors": [l for l in keep if "(EE)" in l][:20]}


def check_zss(raw):
    if not shutil.which("zssctl"):
        return "info", "ZSS is not installed (that is fine for the report)", {"installed": False}
    rc, st = run(["zssctl", "status"])
    raw["zssctl-status.txt"] = st
    d = {"installed": True, "kernel_module": read("/sys/kernel/zss/version")}
    gpu = re.search(r"^(\S+)\s+state=", st, re.M)
    if gpu:
        rc, chk = run(["zssctl", "lend", "--check", gpu.group(1), "--with-group"])
        raw["zssctl-lend-check.txt"] = chk
    return ("ok" if rc == 0 else "warn"), "ZSS installed: " + (st.splitlines()[0] if st else "zssctl gave no answer"), d


# ---- moving tests --------------------------------------------------------------------------
#
# The project's own tests of moving programs between this machine's GPUs. A test
# daemon of their own runs in dry-run mode on a private socket: programs are moved
# from GPU to GPU and back and their frames compared, and no GPU is switched off,
# no driver is touched, nothing needs root. They need a build of ZSS: the one next
# to this program in the kit or the source tree, or ZSS_BUILD.


def zss_build():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.environ.get("ZSS_BUILD"), os.path.join(here, "build"), os.path.join(here, "..", "build")):
        if cand and os.path.exists(os.path.join(cand, "src", "daemon", "zssd")) and \
                os.path.exists(os.path.join(cand, "tests", "zss-testapp")):
            return os.path.abspath(cand)
    return None


def tests_dir():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "tests"), os.path.join(here, "..", "tests")):
        if os.path.exists(os.path.join(cand, "track_a.py")):
            return os.path.abspath(cand)
    return None


# Which of the project's test tracks the report runs, and what each covers.
MOVING_TRACKS = [
    ("track_a.py", "moving programs between GPUs and back, frames compared; real windowed programs"),
    ("track_loss.py", "a GPU lost under a running program: rebuilt on another GPU"),
    ("track_off.py", "switching a GPU off and on with programs moved away (test daemon, no real power change)"),
]


def check_moving(raw):
    if os.environ.get("ZSS_REPORT_NO_TESTS"):
        return "info", "moving tests not run (ZSS_REPORT_NO_TESTS is set)", {}
    build, tests = zss_build(), tests_dir()
    if not build or not tests:
        return "warn", "moving tests not run: no ZSS build next to this program (use the full kit, or build the source)", {}
    env = dict(os.environ, ZSS_BUILD=build)
    for k in ("ZSS_START_ON", "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "ZRN_LAUNCH_ROUTED", "LD_PRELOAD"):
        env.pop(k, None)
    env["ZRN_LAUNCH_ROUTER"] = "off"
    results, passed, failed, skipped = {}, 0, 0, 0
    for track, what in MOVING_TRACKS:
        path = os.path.join(tests, track)
        if not os.path.exists(path):
            continue
        rc, out = run([sys.executable, path], timeout=900, env=env)
        raw[f"test-{track[:-3]}.txt"] = out
        lines = [l for l in out.splitlines() if l[:4] in ("PASS", "FAIL", "SKIP")]
        passed += sum(l.startswith("PASS") for l in lines)
        failed += sum(l.startswith("FAIL") for l in lines)
        skipped += sum(l.startswith("SKIP") for l in lines)
        results[track[:-3]] = {"covers": what, "results": lines}
    status = "fail" if failed else "ok" if passed else "warn"
    return status, f"moving tests: {passed} passed, {failed} failed, {skipped} skipped", results


CHECKS = [
    ("System", check_system),
    ("GPUs", check_gpus),
    ("Power control", None),  # needs the GPU list
    ("Drivers", check_drivers),
    ("IOMMU", check_iommu),
    ("Kernel log", check_kernel_log),
    ("Vulkan", check_vulkan),
    ("OpenGL", check_opengl),
    ("Display server", check_display),
    ("ZSS", check_zss),
    ("Moving tests", check_moving),
]

# ---- the report -----------------------------------------------------------------------------


def collect(progress=None):
    """Runs every check. `progress(name, status, summary)` is called after each."""
    raw, results, gpu_info = {}, [], {"gpus": []}
    for name, fn in CHECKS:
        try:
            if name == "Power control":
                status, summary, detail = check_power(raw, gpu_info)
            else:
                status, summary, detail = fn(raw)
            if name == "GPUs":
                gpu_info = detail
        except Exception as e:  # a check that breaks must not stop the others
            status, summary, detail = "fail", f"the check itself failed: {e}", {}
        results.append({"check": name, "status": status, "summary": summary, "detail": detail})
        if progress:
            progress(name, status, summary)
    return results, raw


def summary_text(results):
    stamp = {"ok": "OK  ", "warn": "WARN", "fail": "FAIL", "info": "    "}
    lines = [f"ZrnSelectiveSuspend tester report (format {VERSION})", ""]
    lines += [f"[{stamp.get(r['status'], '?')}] {r['check']}: {r['summary']}" for r in results]
    return "\n".join(lines)


def write_report(results, raw, out_dir=None, extra=None):
    """Writes the report folder and archive, redacted. Returns (folder, archive)."""
    red = Redactor()
    when = datetime.datetime.now().strftime("%Y%m%d-%H%M")
    base = os.path.join(out_dir or os.path.expanduser("~"), f"zss-report-{when}")
    os.makedirs(os.path.join(base, "raw"), exist_ok=True)
    doc = {"format": VERSION, "created": when, "results": results, "extra": extra or {}}
    text = json.dumps(doc, indent=2, default=str)
    with open(os.path.join(base, "report.json"), "w") as f:
        f.write(red(text))
    with open(os.path.join(base, "summary.txt"), "w") as f:
        f.write(red(summary_text(results)) + "\n")
    for name, content in raw.items():
        with open(os.path.join(base, "raw", name), "w") as f:
            f.write(red(content or "") + "\n")
    archive = base + ".tar.gz"
    with tarfile.open(archive, "w:gz") as t:
        t.add(base, arcname=os.path.basename(base))
    return base, archive


def write_debug_log(results, raw, path, extra=None):
    """The whole report as one text file (redacted), for keeping rather than sending."""
    red = Redactor()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    parts = [summary_text(results), ""]
    for r in results:
        parts.append(f"== {r['check']}: {r['status']}\n" + json.dumps(r["detail"], indent=2, default=str))
    for k, v in (extra or {}).items():
        parts.append(f"== {k}\n" + json.dumps(v, indent=2, default=str))
    for name, content in raw.items():
        parts.append(f"==== {name}\n{content or ''}")
    with open(path, "w") as f:
        f.write(red("\n\n".join(parts)) + "\n")
    return path


def mail_link(results, archive):
    red = Redactor()
    machine = next((r["summary"].split(",")[0] for r in results if r["check"] == "System"), "a machine")
    subject = f"ZSS tester report: {machine}"
    body = (red(summary_text(results)) +
            f"\n\nThe full report is in {red(archive)}. Please attach that file to this mail before sending.\n")
    return "mailto:" + REPORT_TO + "?" + urllib.parse.urlencode({"subject": red(subject), "body": body},
                                                                  quote_via=urllib.parse.quote)


def open_mail(results, archive):
    link = mail_link(results, archive)
    for opener in (["xdg-email"], ["xdg-open"]):
        if shutil.which(opener[0]):
            subprocess.Popen(opener + [link], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return True
    return False


# ---- the optional full test ------------------------------------------------------------------


def source_tree():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    return root if os.path.exists(os.path.join(root, "packaging", "install.sh")) else None


def full_test_steps():
    """The commands the full test runs, in order, for showing before asking."""
    root = source_tree()
    if not root:
        return None
    return [
        ("build", ["sh", "-c", f"cd '{root}' && (meson setup build >/dev/null 2>&1 || true) && ninja -C build"]),
        ("all tests", ["sh", "-c", f"cd '{root}' && meson test -C build --print-errorlogs"]),
        ("install", ["sudo", f"{root}/packaging/install.sh", "--kernel-module", "--no-patch-driver"]),
        ("status", ["zssctl", "status"]),
        ("power off", ["zssctl", "off", "GPU"]),
        ("power on", ["zssctl", "on", "GPU"]),
    ]


def run_full_test(results, log=print):
    """Runs the full test; returns {step: (status, output)}."""
    steps = full_test_steps()
    out = {}
    gpu = next((g["address"] for r in results if r["check"] == "GPUs" for g in r["detail"]["gpus"]
                if g["boot_vga"] != "1"), None)
    for name, cmd in steps or []:
        cmd = [gpu if c == "GPU" else c for c in cmd]
        if "GPU" in cmd or (name.startswith("power") and not gpu):
            out[name] = ("skipped", "no discrete GPU")
            continue
        log(f"--> {name}: {' '.join(cmd)}")
        rc, text = run(cmd, timeout=1800)
        out[name] = ("ok" if rc == 0 else "fail", text[-20000:])
        log(text[-3000:])
        if rc != 0 and name in ("build", "install"):
            break
    return out


# ---- command line ---------------------------------------------------------------------------


def main():
    args = sys.argv[1:]
    if "-h" in args or "--help" in args:
        print(__doc__)
        return 0
    def value(flag):
        return args[args.index(flag) + 1] if flag in args and args.index(flag) + 1 < len(args) else None

    out_dir = value("--out")
    debug_log = value("--debug-log")
    quiet = "--quiet" in args
    if "--no-tests" in args:
        os.environ["ZSS_REPORT_NO_TESTS"] = "1"
    # --include NAME=FILE puts a file into the report (the installer adds its log this way).
    includes = [args[i + 1] for i, a in enumerate(args) if a == "--include" and i + 1 < len(args)]
    if not quiet:
        print("Collecting; nothing on this machine is changed.\n")
    results, raw = collect(None if quiet else lambda n, s, t: print(f"  [{s:4}] {n}: {t}"))
    for inc in includes:
        name, _, path = inc.partition("=")
        raw[name] = read(path, f"{path}: could not be read")
    extra = {}
    if debug_log and not out_dir:
        write_debug_log(results, raw, debug_log)
        if not quiet:
            print(f"\nDebug log written to {debug_log}")
        return 0
    if debug_log:
        write_debug_log(results, raw, debug_log)
    if "--full-test" in args:
        steps = full_test_steps()
        if not steps:
            print("\n--full-test needs the ZSS source tree around this script; skipped.")
        else:
            print("\nThe full test will run, in order:")
            for name, cmd in steps:
                print(f"  {name}: {' '.join(cmd)}")
            print("It installs ZSS (with its kernel module, without patching the GPU driver) and switches the\n"
                  "discrete GPU off and on once. Programs using that GPU are moved or paused meanwhile.")
            if input("Go ahead? [y/N] ").strip().lower() in ("y", "yes"):
                extra["full_test"] = run_full_test(results)
    folder, archive = write_report(results, raw, out_dir, extra)
    if quiet:
        print(archive)
    else:
        print(f"\nReport written to {folder}\nand packed into {archive}")
        print(f"Please look through it, then send the .tar.gz file to {REPORT_TO}.")
    if "--mail" in args:
        print("Opening your mail program..." if open_mail(results, archive) else "No mail program could be opened.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
