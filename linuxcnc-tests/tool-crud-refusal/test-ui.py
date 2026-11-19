#!/usr/bin/env python3
"""A refused tool update or delete fails that command and tells the operator;
the machine goes on as it was."""

import os
import subprocess
import sys

sys.path.insert(0, "../tool-table-load")
from common import *


def tool_cmd(*args):
    return subprocess.run(["./tool-cmd", linuxcnc.nmlfile] + [str(a) for a in args],
                          timeout=30).returncode


def check_running(what):
    s.poll()
    if s.spindle[0]["enabled"] == 0 or s.spindle[0]["direction"] != 1:
        fail("%s: the spindle stopped" % what)
    if s.flood != linuxcnc.FLOOD_ON:
        fail("%s: the coolant stopped" % what)
    if s.task_state != linuxcnc.STATE_ON or s.task_mode != linuxcnc.MODE_MDI:
        fail("%s: state %d, mode %d" % (what, s.task_state, s.task_mode))
    if s.interp_state != linuxcnc.INTERP_IDLE:
        fail("%s: interpreter state %d" % (what, s.interp_state))


def refused(what, message, *args):
    print("*** " + what)
    before = table()
    rc = tool_cmd(*args)
    if rc != 1:
        fail("%s: tool-cmd returned %d, expected the command to fail" % (what, rc))
    time.sleep(0.2)
    check_running(what)
    wait_error(message)
    if table() != before:
        fail("%s: the table file was rewritten:\n%s" % (what, table()))


start()
mdi("M3 S100")
mdi("M8")
check_running("start")

refused("delete of an unknown tool", "can't delete tool 99", "delete", 99)
refused("update of an unknown tool without a pocket", "can't update tool 9",
        "update", 9)
refused("update to a pocket another tool has", "can't update tool 1",
        "update", 1, 2)
mdi("M61 Q1")
refused("delete of the tool in the spindle", "can't delete tool 1", "delete", 1)

print("*** the next command succeeds")
if tool_cmd("delete", 2) != 0:
    fail("delete of T2 did not succeed after the refusals")
wait_for(lambda: 2 not in tools(), "T2 to go")
check_running("after the delete")
if tool_cmd("update", 4, 4) != 0:
    fail("create of T4 did not succeed after the refusals")
wait_for(lambda: 4 in tools(), "T4 to appear")
check_running("after the create")

print("*** a refusal leaves an open program open")
program = os.path.abspath("program.ngc")
c.mode(linuxcnc.MODE_AUTO)
c.wait_complete()
c.program_open(program)
c.wait_complete()
if tool_cmd("delete", 99) != 1:
    fail("delete of an unknown tool in AUTO did not fail")
wait_error("can't delete tool 99")
s.poll()
if s.file != program:
    fail("the program was closed: file is %r" % s.file)
c.auto(linuxcnc.AUTO_RUN, 0)
wait_for(lambda: abs(s.position[0] - 1.0) < 1e-6
         and s.interp_state == linuxcnc.INTERP_IDLE, "the program to run")

print("*** all checks passed")
sys.exit(0)
