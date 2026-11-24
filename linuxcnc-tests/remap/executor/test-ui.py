#!/usr/bin/env python3

import os
import sys
import time

import hal
import linuxcnc


def fail(message):
    print(message)
    sys.exit(1)


def wait_for(predicate, message, timeout=5.0):
    start = time.time()
    while time.time() - start < timeout:
        status.poll()
        if predicate():
            return
        time.sleep(0.01)
    fail(message)


def wait_complete(message, timeout=5.0):
    if command.wait_complete(timeout) != linuxcnc.RCS_DONE:
        fail(message)


def pin_value(name):
    try:
        return hal.get_value(name)
    except RuntimeError:
        return None


def wait_request(executor, opcode):
    prefix = "remap.executor.%s." % executor
    wait_for(
        lambda: hal.get_value(prefix + "request"),
        "executor request was not asserted",
    )
    request_id = int(hal.get_value(prefix + "request-id"))
    if request_id == 0:
        fail("executor used reserved request ID zero")
    if int(hal.get_value(prefix + "opcode")) != opcode:
        fail("executor published the wrong opcode")
    if hal.get_value(prefix + "cancel"):
        fail("executor cancel was asserted for a new request")
    return prefix, request_id


def wait_error(expected, timeout=5.0):
    start = time.time()
    while time.time() - start < timeout:
        message = errors.poll()
        if message is None:
            time.sleep(0.01)
            continue
        kind, text = message
        if kind in (linuxcnc.NML_ERROR, linuxcnc.OPERATOR_ERROR) and expected in text:
            return
        print("ignoring message: %s" % text)
    fail("no error containing %r" % expected)


def wait_interp_idle(message):
    wait_for(lambda: status.interp_state == linuxcnc.INTERP_IDLE, message)


def respond(prefix, request_id, state, result):
    hal.set_p(prefix + "state", str(state))
    hal.set_p(prefix + "result", str(result))
    hal.set_p(prefix + "response-id", str(request_id))


def assert_released(prefix):
    wait_for(
        lambda: not hal.get_value(prefix + "request"),
        "completed executor request was not released",
    )
    if hal.get_value(prefix + "cancel"):
        fail("completed executor request was cancelled")


command = linuxcnc.command()
status = linuxcnc.stat()
errors = linuxcnc.error_channel()
test_component = hal.component("executor-test-ui")
test_component.ready()

wait_for(
    lambda: status.task_state == linuxcnc.STATE_ESTOP,
    "LinuxCNC did not initialize",
)

for pin in (
    "remap.executor.chuck.request-id",
    "remap.executor.chuck.response-id",
    "remap.executor.tailstock.request-id",
    "remap.owner-marker",
):
    wait_for(
        lambda pin=pin: pin_value(pin) is not None,
        "missing caller-owned executor component pin %s" % pin,
    )

if not hal.get_value("remap.owner-marker"):
    fail("executor endpoint did not share its caller-owned HAL component")

command.state(linuxcnc.STATE_ESTOP_RESET)
command.state(linuxcnc.STATE_ON)
wait_complete("machine did not turn on")
command.mode(linuxcnc.MODE_MDI)
wait_complete("machine did not enter MDI")

scenario = os.environ["EXECUTOR_SCENARIO"]

if scenario == "success":
    command.mdi("M10")
    prefix, first_id = wait_request("chuck", 0)
    respond(prefix, first_id, 4, 0)
    wait_complete("successful executor request did not complete")
    assert_released(prefix)

    command.mdi("M11")
    prefix, second_id = wait_request("chuck", 1)
    if second_id != first_id + 1:
        fail("executor request IDs were not monotonic")
    respond(prefix, second_id, 4, 7)
    wait_complete("second successful executor request did not complete")
    assert_released(prefix)

    command.mdi("M20")
    prefix, request_id = wait_request("tailstock", 7)
    respond(prefix, request_id, 5, 42)
    wait_complete("raw executor transaction did not return to its remap")
    assert_released(prefix)
    if int(hal.get_value("remap.raw-result")) != 42:
        fail("custom remap did not receive the raw executor result")

    command.mdi("M21")
    wait_complete("non-HAL executor transport did not complete")
    if int(hal.get_value("remap.transport-result")) != 42:
        fail("executor helper required a HAL transport")

elif scenario == "failure":
    command.mdi("M10")
    prefix, request_id = wait_request("chuck", 0)
    respond(prefix, request_id, 5, 42)
    wait_for(
        lambda: not hal.get_value(prefix + "request"),
        "failed executor request was not released",
    )
    if hal.get_value(prefix + "cancel"):
        fail("terminal executor failure was incorrectly cancelled")
    # Only a succeeded response continues; anything else ends the MDI in error.
    wait_error("executor 'chuck': ")
    wait_interp_idle("failed executor request did not stop the MDI command")

elif scenario == "timeout":
    command.mdi("M11")
    prefix, timed_out_id = wait_request("chuck", 1)
    wait_for(
        lambda: hal.get_value(prefix + "cancel")
        and not hal.get_value(prefix + "request"),
        "timed-out executor request was not cancelled",
        timeout=2.0,
    )
    wait_error("executor 'chuck': timed out")
    wait_interp_idle("timed-out executor request did not stop the MDI command")

    command.abort()
    wait_complete("timeout recovery abort did not complete")
    command.mdi("M10")
    prefix, next_id = wait_request("chuck", 0)
    respond(prefix, timed_out_id, 4, 0)
    time.sleep(0.1)
    if not hal.get_value(prefix + "request"):
        fail("a stale response completed the next executor request")
    respond(prefix, next_id, 4, 0)
    wait_complete("request following a timeout did not complete")
    assert_released(prefix)

elif scenario == "abort":
    command.mdi("M10")
    prefix, unused_request_id = wait_request("chuck", 0)
    command.abort()
    wait_complete("abort command did not complete")
    wait_for(
        lambda: hal.get_value(prefix + "cancel")
        and not hal.get_value(prefix + "request"),
        "aborted executor request was not cancelled",
    )

elif scenario == "readahead":
    # The interpreter reads M10 while the 2 s move before it still runs; the
    # request must not start until the move has finished.
    command.mode(linuxcnc.MODE_AUTO)
    wait_complete("machine did not enter AUTO")
    command.program_open(os.path.join(os.getcwd(), "readahead.ngc"))
    wait_complete("program did not open")
    command.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(lambda: status.current_vel > 0.0, "move before M10 did not start")
    prefix = "remap.executor.chuck."
    while True:
        status.poll()
        if hal.get_value(prefix + "request"):
            break
        if status.interp_state == linuxcnc.INTERP_IDLE:
            fail("program ended without an executor request")
        time.sleep(0.005)
    status.poll()
    if abs(status.position[0] - 4.0) > 0.001 or status.current_vel != 0.0:
        fail("executor request started during the earlier move at X%.4f"
             % status.position[0])
    prefix, request_id = wait_request("chuck", 0)
    respond(prefix, request_id, 4, 0)
    assert_released(prefix)
    wait_interp_idle("program did not finish after the executor request")

elif scenario in ("stop", "pause"):
    # A request already started runs to its end while Stop or Pause holds;
    # the program reads nothing past the remap until Resume.
    command.mode(linuxcnc.MODE_AUTO)
    wait_complete("machine did not enter AUTO")
    command.program_open(os.path.join(os.getcwd(), "hold.ngc"))
    wait_complete("program did not open")
    command.auto(linuxcnc.AUTO_RUN, 0)
    prefix, request_id = wait_request("chuck", 0)
    if scenario == "stop":
        command.program_stop()
        holding = lambda: status.stop_state == linuxcnc.STOP_STOPPING
    else:
        command.auto(linuxcnc.AUTO_PAUSE)
        holding = lambda: status.interp_state == linuxcnc.INTERP_PAUSED
    wait_for(
        lambda: holding() and status.hold_wait == linuxcnc.HOLD_WAIT_EXECUTOR,
        "%s did not wait for the executor request" % scenario,
    )
    time.sleep(0.3)
    if not hal.get_value(prefix + "request") or hal.get_value(prefix + "cancel"):
        fail("%s ended the executor request" % scenario)
    respond(prefix, request_id, 4, 0)
    assert_released(prefix)
    if scenario == "stop":
        wait_for(
            lambda: status.stop_state == linuxcnc.STOP_STOPPED,
            "Stop did not complete after the executor request",
        )
    else:
        wait_for(
            lambda: status.hold_wait == linuxcnc.HOLD_WAIT_NONE
            and status.interp_state == linuxcnc.INTERP_PAUSED,
            "Pause did not hold after the executor request",
        )
    time.sleep(0.3)
    status.poll()
    if abs(status.position[0]) > 0.001:
        fail("%s: program continued past the executor remap" % scenario)
    command.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: status.interp_state == linuxcnc.INTERP_IDLE
        and abs(status.position[0] - 2.0) < 0.001,
        "program did not finish after Resume",
    )

elif scenario == "stop-timeout":
    # The request keeps its timeout while Stop waits for it; a timeout ends
    # the program as usual.
    command.mode(linuxcnc.MODE_AUTO)
    wait_complete("machine did not enter AUTO")
    command.program_open(os.path.join(os.getcwd(), "hold-timeout.ngc"))
    wait_complete("program did not open")
    command.auto(linuxcnc.AUTO_RUN, 0)
    prefix, unused_request_id = wait_request("chuck", 1)
    command.program_stop()
    wait_error("executor 'chuck': timed out")
    wait_for(
        lambda: status.stop_state == linuxcnc.STOP_IDLE
        and status.interp_state == linuxcnc.INTERP_IDLE,
        "timeout during Stop did not end the program",
    )
    if abs(status.position[0]) > 0.001:
        fail("program continued after the executor timed out")

elif scenario == "comp":
    # Under cutter radius compensation the move before the remap is still
    # held back, so the request is refused instead of starting early.
    command.mode(linuxcnc.MODE_AUTO)
    wait_complete("machine did not enter AUTO")
    command.program_open(os.path.join(os.getcwd(), "comp.ngc"))
    wait_complete("program did not open")
    command.auto(linuxcnc.AUTO_RUN, 0)
    wait_error("cutter radius compensation")
    wait_interp_idle("refused executor request did not end the program")
    if hal.get_value("remap.executor.chuck.request"):
        fail("executor request started under cutter radius compensation")

else:
    fail("unknown executor scenario %s" % scenario)

print("executor remap scenario ok: %s" % scenario)
sys.exit(0)
