#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Daemon protocol tests that need no GPU: status, authorisation, bad input."""
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zsstest import Daemon, FAKE_PCI, check, run_scenarios


def status_lists_the_gpu():
    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run")
    try:
        rc, out = d.ctl("status")
        check(rc == 0, out)
        check(FAKE_PCI in out and "state=attached" in out and "backend=dry-run" in out, out)
    finally:
        d.stop()


def unknown_device_is_rejected():
    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run")
    try:
        rc, out = d.ctl("detach", "0000:99:00.0")
        check(rc != 0 and "0000:99:00.0 is not a managed GPU" in out, out)
        rc, out = d.ctl("status")
        check("state=attached" in out, "state changed after a rejected request: " + out)
    finally:
        d.stop()


def unauthorised_detach_changes_nothing():
    if os.geteuid() == 0:
        return "running as root, which is always authorised"
    # Without owner access only root and the admin group may detach; this user is neither.
    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run", "--no-owner-access", "--group", "zss-no-such-group")
    try:
        rc, out = d.ctl("detach", FAKE_PCI)
        check(rc != 0 and "permission denied" in out, "detach was not refused: " + out)
        rc, out = d.ctl("attach", FAKE_PCI)
        check(rc != 0 and "permission denied" in out, "attach was not refused: " + out)
        rc, out = d.ctl("status")
        check(rc == 0 and "state=attached" in out and "dry run" not in out,
              "device changed after an unauthorised request: " + out)
        check("detaching" not in d.log_text(), "daemon started a detach it should have refused")
    finally:
        d.stop()


def garbage_does_not_kill_the_daemon():
    d = Daemon("--gpu", f"{FAKE_PCI}=dry-run")
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(d.socket)
        s.sendall(b"this is not json\n{\"type\":\n" + b"x" * 20000 + b"\n{\"type\":\"nope\"}\n")
        s.close()
        rc, out = d.ctl("status")
        check(rc == 0 and FAKE_PCI in out, "daemon stopped answering after malformed input: " + out)
    finally:
        d.stop()


def no_backend_is_reported():
    # A device with no hot-plug slot and no gmux has no backend; detach must say so.
    d = Daemon("--gpu", FAKE_PCI)
    try:
        rc, out = d.ctl("status")
        check("backend=none" in out, out)
        rc, out = d.ctl("detach", FAKE_PCI)
        check(rc != 0 and "no power backend is available" in out, out)
    finally:
        d.stop()


if __name__ == "__main__":
    sys.exit(run_scenarios([
        ("status lists the managed GPU", status_lists_the_gpu),
        ("detach of an unknown device is rejected", unknown_device_is_rejected),
        ("unauthorised detach and attach change nothing", unauthorised_detach_changes_nothing),
        ("malformed input does not stop the daemon", garbage_does_not_kill_the_daemon),
        ("a device with no power backend is refused", no_backend_is_reported),
    ]))
