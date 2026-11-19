import hal
import linuxcnc
import linuxcnc_util

import math
import socket
import subprocess
import sys
import time

c = linuxcnc.command()
e = linuxcnc.error_channel()
s = linuxcnc.stat()
l = linuxcnc_util.LinuxCNC(command=c, status=s, error=e)


def fail(msg):
    print("ERROR: " + msg)
    sys.exit(1)


def check(what, got, expected):
    print("%s = %.6f (expecting %.6f)" % (what, got, expected))
    if math.fabs(got - expected) > 1e-6:
        fail("%s = %f, expected %f" % (what, got, expected))


def wait_for(predicate, what, timeout=10):
    start = time.time()
    while not predicate():
        if time.time() - start > timeout:
            fail("timeout waiting for " + what)
        time.sleep(0.01)
        s.poll()


def wait_error(text, timeout=10):
    start = time.time()
    while time.time() - start < timeout:
        err = e.poll()
        if err is None:
            time.sleep(0.01)
            continue
        print("(error channel: %s)" % (err[1],))
        if text in err[1]:
            return
    fail("no error containing '%s'" % text)


def tools():
    s.poll()
    return {t.id: t for t in s.tool_table[1:] if t.id > 0}


def table():
    with open("tool.tbl", newline="") as f:
        return f.read()


def saved():
    """The table file's lines, spaces between words normalized."""
    return [" ".join(line.split()) for line in table().split("\n")]


def start():
    l.wait_for_linuxcnc_startup()
    c.state(linuxcnc.STATE_ESTOP_RESET)
    c.state(linuxcnc.STATE_ON)
    mdi_mode()


def mdi_mode():
    c.mode(linuxcnc.MODE_MDI)
    c.wait_complete()


def mdi(cmd):
    c.mdi(cmd)
    c.wait_complete()


def spindle_tool_is(toolno):
    """Status, pocket 0 and the iocontrol pin all show the tool."""
    s.poll()
    return (s.tool_in_spindle == toolno and s.tool_table[0].id == toolno
            and hal.get_value("iocontrol.0.tool-number") == toolno)


def load_table_file(name):
    """Load a named table file; only linuxcncrsh's command takes a name."""
    ini = sys.argv[sys.argv.index("-ini") + 1]
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    rsh = subprocess.Popen(["linuxcncrsh", "-p", str(port), "--", "-ini", ini])
    try:
        start_time = time.time()
        while True:
            try:
                conn = socket.create_connection(("127.0.0.1", port), timeout=10)
                break
            except OSError:
                if time.time() - start_time > 10:
                    fail("linuxcncrsh did not start")
                time.sleep(0.05)
        with conn:
            for line in ("hello EMC test 1.0", "set enable EMCTOO",
                         "set wait_mode done", "set load_tool_table " + name,
                         "get wait_mode"):
                conn.sendall((line + "\r\n").encode())
            # the reply to the last command: the load before it has finished
            reply = b""
            while b"WAIT_MODE DONE" not in reply:
                data = conn.recv(4096)
                if not data:
                    fail("linuxcncrsh closed the connection: %r" % reply)
                reply += data
            print("(linuxcncrsh: %r)" % reply)
    finally:
        rsh.terminate()
        rsh.wait()
