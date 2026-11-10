"""Production snapshot/IPC effects, each in an exclusive user+IPC namespace."""

import ctypes
import json
import fcntl
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import sys
import time
import tempfile

import pytest


SCENARIOS = ["normal", "foreign", "readonly", "forbidden", "kill-own", "alarm-own",
             "parent-death-own", "kill-blocked", "alarm-blocked", "removed", "stale",
             "config-master", "config-fallback", "config-overflow", "config-mode", "reset-owner", "initialize-death", "initialize-normal",
             "initialize-timeout", "reader-exclusive", "config-lease", "initialize-kill", "config-missing-ipc", "initialize-release-window", "timeout-blocked",
             "teardown-attached"]


def wait_until(predicate, seconds=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("fixture effect deadline expired")


def run_case(scenario, directory, reader, fixture, prefix):
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)  # Reap this fixture's orphaned tracee after parent death.
    root = Path(directory)
    config = root / "status.nml"
    lease_directory = tempfile.TemporaryDirectory(dir="/dev/shm", prefix="linuxcnc-snapshot-")
    lease = Path(lease_directory.name) / "status.lock"
    lease.touch(mode=0o640)
    config.write_text(
        f"B emcStatus SHMEM localhost 20480 0 0 2 16 1930000001 TCP=59999 xdr SNAPSHOT_LOCK={lease}\n"
        "P fixture_writer emcStatus LOCAL localhost RW 0 0.1 1 0\n"
        "P xemc emcStatus LOCAL localhost R 0 0.1 0 10\n")
    ini = root / "machine.ini"
    ini.touch()
    env = {**os.environ, "HOME": str(root / "missing-home")}
    writer = subprocess.Popen([*prefix, fixture, "writer", str(config), str(ini)],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, bufsize=1, env=env)
    children = []

    def command(value):
        assert writer.poll() is None, "fixture writer died"
        writer.stdin.write(value + "\n")
        writer.stdin.flush()
        return writer.stdout.readline().strip()

    def value():
        return int(command("value"))

    def execute(mode=None):
        args = [*prefix, fixture, mode, str(config)] if mode else [*prefix, reader, str(config)]
        return subprocess.run(args, capture_output=True, text=True, env=env, timeout=4)

    try:
        assert writer.stdout.readline().strip() == "ready", writer.stderr.read()
        if scenario == "normal":
            beats = []
            for _ in range(3):
                result = execute()
                assert result.returncode == 0, result.stderr
                fields = result.stdout.split()
                assert fields[0] == "linuxcnc-status-v1"
                beats.append(int(fields[1]))
                assert fields[2:5] == ["4", "1", "1"]
                assert bytes.fromhex(fields[5]).decode() == str(ini)
            assert beats[0] < beats[1] < beats[2]
            assert command("pause") == "ok"
            assert value() == 1
            assert not (root / "missing-home").exists()
        elif scenario == "foreign":
            assert command("foreign") == "ok"
            original = command("header")
            assert bytes.fromhex(original).startswith(b"UNRELATED_APPLICATION_STATE")
            result = execute()
            assert result.returncode != 0
            assert command("header") == original
            assert value() == 1
        elif scenario in ("readonly", "forbidden"):
            assert command("pause") == "ok"
            original = command("header")
            result = execute("mapping-write" if scenario == "readonly" else "forbidden")
            assert result.returncode == (-signal.SIGSEGV if scenario == "readonly" else 0), result.stderr
            assert command("header") == original
            assert value() == 1
        elif scenario.startswith("config-"):
            assert command("pause") == "ok"
            original = command("header")
            inventory = [Path(f"/proc/sysvipc/{kind}").read_text() for kind in ("shm", "sem")]
            contents = config.read_text()
            if scenario == "config-master":
                contents = contents.replace("R 0 0.1 0 10", "R 0 0.1 1 10")
            elif scenario == "config-fallback":
                contents = contents.replace("P xemc emcStatus LOCAL localhost R 0 0.1 0 10",
                    " P xemc emcStatus LOCAL localhost R 0 0.1 0 10\nP xemc default REMOTE localhost R 0 0.1 0 10")
            elif scenario == "config-overflow":
                contents = contents.replace("1930000001", "4294967296")
            elif scenario == "config-missing-ipc":
                contents = contents.replace("1930000001", "1930000002")
            elif scenario == "config-lease":
                contents = contents.replace(f" SNAPSHOT_LOCK={lease}", "")
            else:
                contents = contents.replace("TCP=59999 xdr", "TCP=59999 xdr MUTEX=NONE")
            config.write_text(contents)
            result = execute()
            assert result.returncode != 0
            assert command("header") == original and value() == 1
            assert [Path(f"/proc/sysvipc/{kind}").read_text() for kind in ("shm", "sem")] == inventory
        elif scenario == "timeout-blocked":
            assert command("lock") == "ok"
            started = time.monotonic()
            result = execute("api-read")
            assert result.returncode != 0 and 0.8 <= time.monotonic() - started < 2.5
            assert value() == 0, "timed-out reader released another owner's hold"
            assert command("unlock") == "ok" and value() == 1
        elif scenario == "reader-exclusive":
            assert command("pause") == "ok"
            with lease.open("rb") as exclusive:
                fcntl.flock(exclusive, fcntl.LOCK_EX)
                result = execute()
                assert result.returncode != 0 and value() == 1
            assert execute("api-read").returncode == 0
        elif scenario == "initialize-kill":
            assert command("pause") == "ok"
            original = command("header")
            initializing = subprocess.Popen(["strace", "-f", "-o", str(root / "initializer.trace"),
                "-e", "inject=shmget:delay_enter=2s:when=1",
                *prefix, fixture, "initialize", str(config)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, start_new_session=True, env=env)
            children.append(initializing)
            def exclusive_held():
                with lease.open("rb") as probe:
                    try:
                        fcntl.flock(probe, fcntl.LOCK_SH | fcntl.LOCK_NB)
                        return False
                    except BlockingIOError:
                        return True
            wait_until(exclusive_held)
            assert execute("api-read").returncode != 0
            os.killpg(initializing.pid, signal.SIGKILL)
            initializing.communicate(timeout=3)
            assert execute("api-read").returncode == 0
            assert value() == 1 and command("header") == original
        elif scenario == "initialize-timeout":
            assert command("pause") == "ok"
            original = command("header")
            with lease.open("rb") as shared:
                fcntl.flock(shared, fcntl.LOCK_SH)
                started = time.monotonic()
                result = execute("initialize")
                elapsed = time.monotonic() - started
                assert result.returncode != 0 and 1.8 <= elapsed < 3.5
                assert value() == 1 and command("header") == original
        elif scenario in ("initialize-death", "initialize-normal", "initialize-release-window"):
            assert command("pause") == "ok"
            baseline = int(command("attachments"))
            trace = root / "reader.trace"
            delay = "2s" if scenario == "initialize-death" else "300ms"
            injections = ["-e", f"inject=semtimedop:delay_exit={delay}:when=1"]
            if scenario == "initialize-release-window":
                injections = ["-e", "inject=semop:delay_enter=300ms:when=1",
                              "-e", "inject=semtimedop:delay_enter=300ms:when=2"]
            traced = subprocess.Popen(["strace", "-f", "-o", str(trace), *injections,
                *prefix, fixture, "api-read", str(config)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                start_new_session=True, env=env)
            children.append(traced)
            wait_until(lambda: value() == 0)
            assert int(command("attachments")) == baseline + 1
            initializing = subprocess.Popen(["strace", "-f", "-o", str(root / "initializer.trace"),
                "-e", "inject=shmget:delay_enter=1s:when=1",
                *prefix, fixture, "initialize", str(config)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, start_new_session=True, env=env)
            children.append(initializing)
            # The actual master constructor is waiting for EX; it must not
            # initialize IPC while the real reader retains its SH lease.
            time.sleep(0.06)
            assert initializing.poll() is None and value() == 0
            if scenario == "initialize-death":
                os.killpg(traced.pid, signal.SIGKILL)
            traced.communicate(timeout=3)

            def master_has_exclusive():
                with lease.open("rb") as probe:
                    try:
                        fcntl.flock(probe, fcntl.LOCK_SH | fcntl.LOCK_NB)
                        return False
                    except BlockingIOError:
                        return True

            wait_until(master_has_exclusive)
            # Master is frozen before its first shmget after acquiring EX.
            # Both undo/release and detach must be complete at this handoff.
            assert value() == 1
            assert int(command("attachments")) == baseline
            assert execute("api-read").returncode != 0
            assert value() == 1
            assert initializing.stdout.readline().strip() == "initialized"
            assert value() == 1
            initializing.communicate("stop\n", timeout=3)
            assert initializing.returncode == 0
        elif scenario == "teardown-attached":
            # The last master exits while a real reader is still attached; the
            # private IPC namespace must end with no segment or semaphore.
            assert command("pause") == "ok"
            trace = root / "reader.trace"
            traced = subprocess.Popen(["strace", "-f", "-o", str(trace),
                "-e", "inject=semtimedop:delay_exit=300ms:when=1",
                *prefix, fixture, "api-read", str(config)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                start_new_session=True, env=env)
            children.append(traced)
            wait_until(lambda: value() == 0)
            command("stop")
            writer.communicate(timeout=5)
            traced.communicate(timeout=3)
            for kind in ("shm", "sem"):
                remaining = Path(f"/proc/sysvipc/{kind}").read_text().splitlines()[1:]
                assert not remaining, f"{kind} left behind: {remaining}"
        elif scenario == "stale":
            assert command("pause") == "ok"
            result = execute()
            assert result.returncode != 0 and "stale" in result.stderr
            assert value() == 1
        else:
            blocked = scenario.endswith("blocked")
            assert command("lock" if blocked else "pause") == "ok"
            mode = "alarm-read" if scenario.startswith("alarm") else "api-read"
            program = [*prefix, fixture, mode, str(config)]
            if scenario == "parent-death-own":
                program = [*prefix, reader, str(config)]
            trace = root / "syscalls.trace"
            # Delay only this reader's syscall return, while the real kernel
            # adjustment is outstanding. No production test hook changes locks.
            injection = "inject=semtimedop:delay_exit=2s:when=1"
            if scenario == "reset-owner":
                injection = "inject=semtimedop:delay_exit=200ms:when=1"
            traced = subprocess.Popen(["strace", "-f", "-o", str(trace), "-e", injection, *program],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                      start_new_session=True, env=env)
            children.append(traced)
            wait_until(lambda: trace.exists() and "semtimedop(" in trace.read_text())
            if not blocked:
                wait_until(lambda: value() == 0)
            if scenario == "reset-owner":
                assert command("reset") == "ok" and command("lock") == "ok"
                traced.wait(timeout=3)
                assert value() == 0, "observer released replacement owner"
                assert command("unlock") == "ok"
            elif scenario == "removed":
                assert command("remove") == "ok"
                traced.wait(timeout=3)
                assert traced.returncode != 0
            elif scenario == "parent-death-own":
                descendants = Path(f"/proc/{traced.pid}/task/{traced.pid}/children").read_text().split()
                assert descendants, "native reader child was not established"
                tracee = int(descendants[0])
                os.kill(traced.pid, signal.SIGKILL)
                wait_until(lambda: value() == 1)
                os.waitpid(tracee, 0)
            elif scenario.startswith("alarm"):
                traced.wait(timeout=3)
            else:
                os.killpg(traced.pid, signal.SIGKILL)
            traced.communicate(timeout=3)
            if scenario != "removed":
                wait_until(lambda: value() == (0 if blocked else 1))
                time.sleep(0.05)
                assert value() == (0 if blocked else 1), "observer over-released a semaphore"
                if blocked:
                    assert command("unlock") == "ok"
                    assert value() == 1
            else:
                assert value() == -1, "removed semaphore was recreated"
    finally:
        for child in children:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.communicate(timeout=3)
        if writer.poll() is None:
            command("stop")
        writer.communicate(timeout=3)
        lease_directory.cleanup()


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_passive_native_status_effects(tmp_path, scenario):
    reader = os.environ.get("LINUXCNC_SNAPSHOT_READER")
    fixture = os.environ.get("LINUXCNC_SNAPSHOT_FIXTURE")
    if not reader or not fixture or not shutil.which("strace") or not shutil.which("unshare"):
        pytest.skip("passive effects require built reader/fixture, strace and private namespaces")
    prefix = os.environ.get("LINUXCNC_SNAPSHOT_EXEC_PREFIX", "[]")
    result = subprocess.run([
        "unshare", "--user", "--map-root-user", "--ipc", sys.executable, __file__,
        scenario, str(tmp_path), reader, fixture, prefix,
    ], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr


def run_suite(reader, fixture):
    if not shutil.which("strace") or not shutil.which("unshare"):
        return 77
    probe = subprocess.run(["unshare", "--user", "--map-root-user", "--ipc", "true"],
                           capture_output=True, timeout=5)
    if probe.returncode != 0:
        return 77
    for scenario in SCENARIOS:
        with tempfile.TemporaryDirectory(prefix="linuxcnc-passive-") as directory:
            subprocess.run(["unshare", "--user", "--map-root-user", "--ipc", sys.executable,
                            "-B", __file__, scenario, directory, reader, fixture, "[]"],
                           check=True, timeout=15)
    return 0


if __name__ == "__main__":
    if sys.argv[1] == "--suite":
        sys.exit(run_suite(sys.argv[2], sys.argv[3]))
    run_case(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], json.loads(sys.argv[5]))
