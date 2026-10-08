#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the installer and for the NVIDIA patch tool.

Nothing here touches the running system: the installer works in a staging
root, and the patch tool works on a private copy of the driver source with
DKMS switched off. The patch-tool tests are skipped where the validated
NVIDIA driver source is not installed.
"""
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zsstest import BUILD, check, run_scenarios

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(REPO, "packaging")
TOOL = os.path.join(PKG, "zss-nvidia-patch")
PATCHES = os.path.join(REPO, "patches")
STOCK_SRC = "/usr/src/nvidia-470.256.02"
work = tempfile.mkdtemp(prefix="zss-pkg-", dir="/var/tmp")


def run(*cmd, **kw):
    r = subprocess.run(list(cmd), capture_output=True, text=True, timeout=300, **kw)
    return r.returncode, r.stdout + r.stderr


def tool(src, *args):
    return run(TOOL, *args, "--source", src, "--patch-dir", PATCHES, "--stock-dir", os.path.join(work, "stock"), "--no-dkms")


def source_copy(name):
    dst = os.path.join(work, name)
    shutil.copytree(STOCK_SRC, dst, symlinks=True)
    # On a machine where ZSS is installed the driver source is already registered; start from it as shipped.
    kept = os.path.join(dst, "dkms.conf.zss-orig")
    if os.path.exists(kept):
        os.replace(kept, os.path.join(dst, "dkms.conf"))
        os.remove(os.path.join(dst, "patches", "zss-wake-on-touch.patch"))
        if not os.listdir(os.path.join(dst, "patches")):
            os.rmdir(os.path.join(dst, "patches"))
    return dst


def tree(root):
    found = []
    for base, dirs, files in os.walk(root):
        found += [os.path.relpath(os.path.join(base, f), root) for f in files]
    return sorted(found)


def patch_register_and_remove():
    if not os.path.isdir(STOCK_SRC):
        return "the validated NVIDIA driver source is not installed"
    src = source_copy("src1")
    shipped = tree(src)
    original = open(os.path.join(src, "dkms.conf")).read()

    rc, out = tool(src, "status")
    check(rc == 0 and "470.256.02 (validated)" in out and "registered with DKMS: no" in out, out)
    rc, out = tool(src, "register")
    check(rc == 0, "register failed: " + out)
    conf = open(os.path.join(src, "dkms.conf")).read()
    check('PATCH[0]="zss-wake-on-touch.patch"' in conf and conf.startswith(original), "dkms.conf was not extended as expected")
    check(filecmp.cmp(os.path.join(src, "patches", "zss-wake-on-touch.patch"),
                      os.path.join(PATCHES, "nvidia-470.256.02-wake-on-touch.patch"), shallow=False), "the patch was not copied")
    # The source itself stays as shipped; DKMS patches its own copy at build time.
    check("nv_zss_request_wake" not in open(os.path.join(src, "nvidia", "nv.c")).read(), "the source tree itself was patched")
    check("registered with DKMS: yes" in tool(src, "status")[1], "status does not show the registration")
    rc, out = tool(src, "register")
    check(rc == 0 and "already registered" in out and open(os.path.join(src, "dkms.conf")).read() == conf,
          "registering twice changed something: " + out)

    # What DKMS will do with it: apply with -p1 in a copy of the source.
    rc, out = run("patch", "-p1", "-s", "-d", src, "-i", os.path.join(src, "patches", "zss-wake-on-touch.patch"))
    check(rc == 0, "the registered patch does not apply the way DKMS applies it: " + out)
    check("nv_zss_request_wake" in open(os.path.join(src, "nvidia", "nv.c")).read(), "patch applied but changed nothing")
    run("patch", "-R", "-p1", "-s", "-d", src, "-i", os.path.join(src, "patches", "zss-wake-on-touch.patch"))

    rc, out = tool(src, "remove")
    check(rc == 0, "remove failed: " + out)
    check(open(os.path.join(src, "dkms.conf")).read() == original, "dkms.conf is not byte-for-byte as shipped after removal")
    check(not os.path.exists(os.path.join(src, "patches")) and not os.path.exists(os.path.join(src, "dkms.conf.zss-orig")),
          "removal left files behind")
    check(tree(src) == shipped, "the source tree differs from the shipped one after removal")
    check("nothing to remove" in tool(src, "remove")[1], "a second removal did something")


def patch_refuses_what_it_does_not_know():
    if not os.path.isdir(STOCK_SRC):
        return "the validated NVIDIA driver source is not installed"
    other = source_copy("src2")
    conf = os.path.join(other, "dkms.conf")
    text = open(conf).read().replace('PACKAGE_VERSION="470.256.02"', 'PACKAGE_VERSION="470.999.99"')
    open(conf, "w").write(text)
    rc, out = tool(other, "register")
    check(rc != 0 and "470.999.99 has not been validated" in out, "an unvalidated version was accepted: " + out)
    check(open(conf).read() == text and not os.path.exists(os.path.join(other, "patches")), "an unvalidated driver was modified")
    check("not validated" in tool(other, "status")[1], "status does not say the version is unvalidated")

    changed = source_copy("src3")
    nv = os.path.join(changed, "nvidia", "nv.c")
    open(nv, "w").write(open(nv).read().replace("struct rw_semaphore nv_system_pm_lock;", "struct rw_semaphore nv_pm_lock_renamed;"))
    before = open(os.path.join(changed, "dkms.conf")).read()
    rc, out = tool(changed, "register")
    check(rc != 0 and "does not apply" in out, "a patch that cannot apply was registered: " + out)
    check(open(os.path.join(changed, "dkms.conf")).read() == before and not os.path.exists(os.path.join(changed, "patches")),
          "a source the patch does not fit was modified")


def installer_check_changes_nothing():
    rc, out = run(os.path.join(PKG, "install.sh"), "--check")
    check(rc == 0 and "what this machine supports" in out and "Application migration" in out, out)
    for line in ("Power-off", "Wake on demand", "Driver in initial ramdisk", "ZSS_Interceptor (kernel)", "IOMMU"):
        check(line in out, f"the report has no '{line}' line:\n{out}")


def install_into_a_staging_root_and_remove():
    root = os.path.join(work, "root")
    os.makedirs(root)
    rc, out = run(os.path.join(PKG, "install.sh"), "--build", BUILD, "--destdir", root)
    check(rc == 0, "install failed: " + out)
    check("the group, the services and the driver were not touched" in out, out)
    for path in ("usr/local/sbin/zssd", "usr/local/bin/zssctl", "usr/local/bin/zss-run", "usr/local/sbin/zss-nvidia-patch",
                 "usr/local/lib/zss/libzss_airlock.so", "usr/local/share/zss/zss_icd.json",
                 "usr/local/share/zss/patches/validated-versions", "etc/zss/zssd.conf", "etc/systemd/system/zssd.service",
                 "etc/systemd/system/zss-nvidia-check.service", "etc/pacman.d/hooks/65-zss-nvidia-patch.hook"):
        check(os.path.exists(os.path.join(root, path)), f"{path} was not installed")
    manifest = open(os.path.join(root, "usr/local/share/zss/zss_icd.json")).read()
    check('"/usr/local/lib/zss/libzss_airlock.so"' in manifest, "the manifest does not point at the installed library")
    launcher = open(os.path.join(root, "usr/local/bin/zss-run")).read()
    check("/usr/local/share/zss/zss_icd.json" in launcher and "VK_DRIVER_FILES" in launcher, "the launcher is wrong")
    check(not os.path.exists(os.path.join(root, "usr/share/vulkan")), "the layer was put where every application would load it")
    check(not os.path.exists(os.path.join(root, "boot")) and not os.path.exists(os.path.join(root, "etc/default")),
          "the installer wrote bootloader files")
    unit = open(os.path.join(root, "etc/systemd/system/zssd.service")).read()
    check("ExecStopPost=/usr/local/sbin/zssd --recover" in unit, "the service has no recovery step")
    check("modprobe -q zss" in unit and "ExecStartPre=-" in unit, "the service does not load the module, or fails without it")
    check(not os.path.exists(os.path.join(root, "usr/src")), "the kernel module was installed without being asked for")
    check(not os.path.exists(os.path.join(root, "etc/modules-load.d")) and not os.path.exists(os.path.join(root, "etc/mkinitcpio.conf.d")),
          "the installer arranged for the module to load at early boot")

    # The installed daemon reads the installed configuration format.
    conf = os.path.join(root, "etc/zss/zssd.conf")
    r = subprocess.run([os.path.join(root, "usr/local/sbin/zssd"), "--config", conf, "--gpu", "ffff:00:00.0=dry-run",
                        "--runtime-dir", work, "--recover"], capture_output=True, text=True, timeout=20)
    check(r.returncode == 0, "the shipped configuration file is not accepted: " + r.stderr)

    # A second install keeps an edited configuration.
    open(conf, "a").write("idle_timeout = 123\n")
    rc, out = run(os.path.join(PKG, "install.sh"), "--build", BUILD, "--destdir", root)
    check(rc == 0 and "idle_timeout = 123" in open(conf).read(), "reinstalling overwrote the configuration")

    # Asked for, the module's source lands where DKMS expects it, complete.
    rc, out = run(os.path.join(PKG, "install.sh"), "--build", BUILD, "--destdir", root, "--kernel-module")
    src = os.path.join(root, "usr/src/zss-0.2.1")
    check(rc == 0 and sorted(os.listdir(src)) == ["Kbuild", "Makefile", "dkms.conf", "zss.c"], "module source not staged: " + out)
    check('PACKAGE_VERSION="0.2.1"' in open(os.path.join(src, "dkms.conf")).read(), "dkms.conf and the directory disagree")

    rc, out = run(os.path.join(PKG, "uninstall.sh"), "--destdir", root)
    check(rc == 0 and tree(root) == ["etc/zss/zssd.conf"], f"uninstall left more than the configuration: {tree(root)}")
    rc, out = run(os.path.join(PKG, "uninstall.sh"), "--destdir", root, "--purge")
    check(rc == 0 and tree(root) == [], f"purge left files: {tree(root)}")


if __name__ == "__main__":
    try:
        rc = run_scenarios([
            ("patch tool: register, what DKMS will apply, remove byte-for-byte", patch_register_and_remove),
            ("patch tool: refuses unvalidated versions and sources it does not fit", patch_refuses_what_it_does_not_know),
            ("installer --check reports and changes nothing", installer_check_changes_nothing),
            ("install into a staging root, reinstall, uninstall, purge", install_into_a_staging_root_and_remove),
        ])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(rc)
