# SPDX-License-Identifier: GPL-2.0-only
"""Shared helpers for the ZSS test harnesses."""
import atexit
import os
import shutil
import subprocess
import tempfile
import time

BUILD = os.environ.get("ZSS_BUILD") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")
BUILD = os.path.abspath(BUILD)
ZSSD = os.path.join(BUILD, "src/daemon/zssd")
ZSSCTL = os.path.join(BUILD, "src/zssctl/zssctl")
ZSS_RUN = os.path.join(BUILD, "src/layer/zss-run")
TESTAPP = os.path.join(BUILD, "tests/zss-testapp")
FAKE_PCI = "ffff:00:00.0"  # for software-only scenarios; no such device exists


# Keep the retention store of test runs out of the user's real cache.
CACHE = tempfile.mkdtemp(prefix="zss-cache-")
atexit.register(shutil.rmtree, CACHE, True)


class Failure(Exception):
    pass


def check(cond, message):
    if not cond:
        raise Failure(message)


def quiet_env(extra=None):
    env = dict(os.environ)
    env.pop("ZSS_DEBUG", None)
    env["XDG_CACHE_HOME"] = CACHE
    # The tests pick their GPU by name, among all of them; the launcher's default is the dedicated one only.
    env["ZSS_START_ON"] = "any"
    env["ZSS_QUIET"] = "1"
    if extra:
        env.update(extra)
    return env


class Daemon:
    """A zssd instance on a private socket."""

    def __init__(self, *args, runtime_dir=None, daemon_env=None):
        # Unix socket paths are short; keep this one out of deep directories.
        base = runtime_dir or os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
        self.dir = tempfile.mkdtemp(prefix="zss-", dir=base)
        self.socket = os.path.join(self.dir, "s")
        self.log = open(os.path.join(self.dir, "zssd.log"), "w+")
        # The file is given explicitly so a system-wide /etc/zss/zssd.conf cannot leak into a test.
        self.conf = os.path.join(self.dir, "zssd.conf")
        open(self.conf, "w").close()
        self.proc = subprocess.Popen([ZSSD, "--config", self.conf, "--socket", self.socket,
                                      "--runtime-dir", self.dir, *args],
                                     stdout=self.log, stderr=subprocess.STDOUT, env=daemon_env)
        deadline = time.time() + 5
        while not os.path.exists(self.socket):
            check(self.proc.poll() is None and time.time() < deadline,
                  "zssd did not start: " + self.log_text())
            time.sleep(0.02)

    def env(self, extra=None):
        return quiet_env({"ZSS_SOCKET": self.socket, **(extra or {})})

    def ctl(self, *args, timeout=180):
        r = subprocess.run([ZSSCTL, *map(str, args)], env=self.env(), capture_output=True, text=True,
                           timeout=timeout)
        return r.returncode, r.stdout + r.stderr

    def log_text(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()
        shutil.rmtree(self.dir, ignore_errors=True)


class App:
    """zss-testapp (or any program) running under the layer."""

    def __init__(self, daemon, args, extra_env=None, program=None, layered=True, run_args=()):
        cmd = [program or TESTAPP, *map(str, args)]
        if layered:
            cmd = [ZSS_RUN, *run_args] + cmd
        env = daemon.env(extra_env) if daemon else quiet_env(extra_env)
        self.proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.lines = []

    @property
    def pid(self):
        return self.proc.pid

    def wait_line(self, wanted, timeout=60):
        """Reads output until a line equal to `wanted` appears."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise Failure(f"application exited before printing '{wanted}': {self.proc.stderr.read()[-600:]}")
            self.lines.append(line.strip())
            if line.strip() == wanted:
                return
        raise Failure(f"timed out waiting for '{wanted}'")

    def wait_containing(self, part, timeout=60):
        """Reads output until a line containing `part` appears."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise Failure(f"application exited before printing '{part}': {self.proc.stderr.read()[-600:]}")
            self.lines.append(line.strip())
            if part in line:
                return
        raise Failure(f"timed out waiting for '{part}'")

    def wait_frame(self, n, timeout=60):
        self.wait_line(f"frame {n}", timeout)

    def last_frame(self):
        frames = [int(l.split()[1]) for l in self.lines if l.startswith("frame ")]
        return frames[-1] if frames else -1

    def finish(self, timeout=120):
        try:
            out, err = self.proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise Failure("application did not finish")
        self.lines += out.split("\n")
        self.stderr = err
        return self.proc.returncode, err

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()

    def open_device_files(self, needle):
        """Paths this process has open that contain `needle`."""
        found = []
        fd_dir = f"/proc/{self.pid}/fd"
        try:
            for name in os.listdir(fd_dir):
                try:
                    target = os.readlink(os.path.join(fd_dir, name))
                except OSError:
                    continue
                if needle in target:
                    found.append(target)
        except OSError:
            pass
        return found


def reference_frames(gpu, frames, out_dir, extra_env=None, extra_args=()):
    """Renders the reference: the same scene with no layer and no migration."""
    os.makedirs(out_dir, exist_ok=True)
    r = subprocess.run([TESTAPP, "--gpu", gpu, "--frames", str(frames), "--out", out_dir, *map(str, extra_args)],
                       env=quiet_env(extra_env), capture_output=True, text=True, timeout=300)
    check(r.returncode == 0, f"reference run on {gpu} failed: {r.stderr[-400:]}")


def layered_gpus(extra_env=None):
    """Device names the layer offers on this machine."""
    r = subprocess.run([ZSS_RUN, TESTAPP, "--list"], env=quiet_env({"ZSS_SOCKET": "/nonexistent", **(extra_env or {})}),
                       capture_output=True, text=True, timeout=120)
    return [l for l in r.stdout.split("\n") if l]


def run_scenarios(scenarios):
    """Runs (name, function) pairs. A function returns None, or a string to skip."""
    failed = 0
    for name, fn in scenarios:
        try:
            skipped = fn()
            print(f"{'SKIP' if skipped else 'PASS'}  {name}" + (f" ({skipped})" if skipped else ""), flush=True)
        except Failure as e:
            failed += 1
            print(f"FAIL  {name}: {e}", flush=True)
    return 1 if failed else 0
