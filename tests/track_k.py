#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Track K: the kernel module, in a QEMU guest.

Builds kmod/zss.ko for the running kernel, boots the same guest as Track B
(the host's kernel, a bochs-display card on a hot-pluggable port) and runs
tests/track_k_guest.py in it. Nothing is loaded into the host's kernel.

Usage: track_k.py [--keep] [--verbose]
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.abspath(os.environ.get("ZSS_BUILD") or os.path.join(HERE, "..", "build"))
KDIR = f"/usr/lib/modules/{os.uname().release}/build"


def build_module():
    if not os.path.exists(os.path.join(KDIR, "Makefile")):
        return f"no kernel build directory at {KDIR}"
    out = os.path.join(BUILD, "kmod")
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "..", "kmod"), out,
                    ignore=shutil.ignore_patterns("*.o", "*.ko", "*.mod*", ".*", "Module.symvers", "modules.order"))
    r = subprocess.run(["make", "-C", out, f"KDIR={KDIR}", "ZSS_TEST_HOOKS=m"], capture_output=True, text=True)
    if r.returncode != 0 or "warning" in (r.stdout + r.stderr).replace("the compiler differs", ""):
        print("FAIL  the module does not build cleanly:\n" + (r.stdout + r.stderr)[-3000:])
        sys.exit(1)
    return None


if __name__ == "__main__":
    why = build_module()
    if why:
        print(f"SKIP  track K ({why})")
        sys.exit(77)
    os.environ["ZSS_BUILD"] = BUILD
    os.environ["ZSS_GUEST_SCRIPT"] = os.path.join(HERE, "track_k_guest.py")
    sys.path.insert(0, HERE)
    import track_b
    sys.exit(track_b.main())
