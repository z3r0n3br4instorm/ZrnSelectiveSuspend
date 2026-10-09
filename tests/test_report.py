#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The tester report: it is written, it reads, and nothing that names the machine or the person is in it."""
import json
import os
import pwd
import socket
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tester", "zss_report.py")
failures = 0


def check(name, cond, detail=""):
    global failures
    print(("PASS  " if cond else "FAIL  ") + name + ("" if cond else f": {detail}"))
    failures += not cond


with tempfile.TemporaryDirectory() as out:
    # The moving tests are the test tracks the suite runs anyway.
    r = subprocess.run([sys.executable, TOOL, "--out", out], capture_output=True, text=True, timeout=600,
                       env=dict(os.environ, ZSS_REPORT_NO_TESTS="1"))
    check("the report is written", r.returncode == 0 and any(n.endswith(".tar.gz") for n in os.listdir(out)), r.stderr)
    folder = next(os.path.join(out, n) for n in os.listdir(out) if not n.endswith(".tar.gz"))
    doc = json.load(open(os.path.join(folder, "report.json")))
    names = [x["check"] for x in doc["results"]]
    check("every check has a result", len(names) >= 10 and all(x["status"] in ("ok", "warn", "fail", "info")
                                                              for x in doc["results"]), names)
    secrets = [socket.gethostname(), pwd.getpwuid(os.getuid()).pw_name, os.path.expanduser("~")]
    leaks = []
    for root, _, files in os.walk(folder):
        for f in files:
            text = open(os.path.join(root, f), errors="replace").read()
            leaks += [(f, s) for s in secrets if s and len(s) > 2 and s in text]
    check("no machine name, user name or home folder in the report", not leaks, leaks[:5])

sys.path.insert(0, os.path.join(HERE, "..", "tester"))
import zss_report  # noqa: E402

red = zss_report.Redactor()
sample = ("root=PARTUUID=a3867d37-2b04-4847-a8c4-7ed8775925a6 Device Serial Number 00-11-22-33-44-55-66-77 "
          "link/ether 00:1b:44:11:3a:b7 inet 192.168.1.20 Serial#: 12345 someone@example.org")
cleaned = red(sample)
check("identifiers are replaced", all(x not in cleaned for x in ("a3867d37", "00-11-22", "00:1b:44", "192.168", "12345",
                                                                 "someone@")), cleaned)
link = zss_report.mail_link([{"check": "System", "summary": "Vendor Model, Distro", "status": "info", "detail": {}}],
                            "/tmp/x.tar.gz")
check("the mail link is addressed to the report address", link.startswith("mailto:" + zss_report.REPORT_TO), link[:80])
sys.exit(1 if failures else 0)
