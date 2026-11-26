#!/usr/bin/env python3

import time

import hal
import linuxcnc


HOME_IDLE = 0
HOME_UNLOCK_WAIT = 3
HOME_FINAL_MOVE_START = 20
HOME_LOCK_WAIT = 23
TIMEOUT = 5.0


command = linuxcnc.command()
status = linuxcnc.stat()
errors = linuxcnc.error_channel()
component = hal.component("homing-concurrent-test")
for joint in range(7):
    component.newpin("is-unlocked-%d" % joint, hal.HAL_BOOL, hal.HAL_OUT)
component.newpin("joint-5-home-switch", hal.HAL_BOOL, hal.HAL_OUT)
component.ready()
for joint in range(7):
    signal = "homing-concurrent-test-unlocked-%d" % joint
    hal.new_sig(signal, hal.HAL_BOOL)
    hal.connect(
        "homing-concurrent-test.is-unlocked-%d" % joint,
        signal,
    )
    hal.connect("joint.%d.is-unlocked" % joint, signal)
hal.new_sig("homing-concurrent-test-joint-5-home-switch", hal.HAL_BOOL)
hal.connect(
    "homing-concurrent-test.joint-5-home-switch",
    "homing-concurrent-test-joint-5-home-switch",
)
hal.connect("joint.5.home-sw-in", "homing-concurrent-test-joint-5-home-switch")


def wait(predicate, description, timeout=TIMEOUT, poll_interval=0.01):
    deadline = time.time() + timeout
    while time.time() < deadline:
        status.poll()
        if predicate():
            return
        time.sleep(poll_interval)
    raise AssertionError("timeout waiting for %s" % description)


def state(joint):
    return hal.get_value("joint.%d.home-state" % joint)


def homing(joint):
    return status.joint[joint]["homing"]


def issue(function, expected=linuxcnc.RCS_DONE):
    function()
    result = command.wait_complete(TIMEOUT)
    assert result == expected, "command returned %s, expected %s" % (result, expected)


def wait_error(fragment):
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        error = errors.poll()
        if error is not None and fragment in error[1]:
            return
        time.sleep(0.01)
    raise AssertionError("missing error containing %r" % fragment)


def wait_for_start(*joints):
    wait(
        lambda: all(state(joint) == HOME_UNLOCK_WAIT for joint in joints),
        "joints %s to wait for unlock" % (joints,),
    )


def finish_joint(joint, wait_for_published_completion=True):
    component["is-unlocked-%d" % joint] = True
    wait(lambda: state(joint) == HOME_LOCK_WAIT, "joint %d lock wait" % joint)
    component["is-unlocked-%d" % joint] = False
    wait(
        lambda: state(joint) == HOME_IDLE
        and status.joint[joint]["homed"]
        and (not wait_for_published_completion or not homing(joint)),
        "joint %d homing completion" % joint,
    )


def finish_synchronized_group():
    wait_for_start(3, 4)
    component["is-unlocked-3"] = True
    wait(lambda: state(3) == HOME_FINAL_MOVE_START, "joint 3 synchronized wait")
    time.sleep(0.05)
    assert state(3) == HOME_FINAL_MOVE_START
    assert state(4) == HOME_UNLOCK_WAIT
    component["is-unlocked-4"] = True
    wait(
        lambda: state(3) == HOME_LOCK_WAIT and state(4) == HOME_LOCK_WAIT,
        "synchronized final moves",
    )
    component["is-unlocked-3"] = False
    component["is-unlocked-4"] = False
    wait(
        lambda: all(
            state(joint) == HOME_IDLE
            and status.joint[joint]["homed"]
            and not homing(joint)
            for joint in (3, 4)
        ),
        "synchronized group completion",
        poll_interval=0.0001,
    )


wait(
    lambda: status.task_state == linuxcnc.STATE_ESTOP
    and status.exec_state == linuxcnc.EXEC_DONE,
    "LinuxCNC startup",
    timeout=10.0,
)
issue(lambda: command.state(linuxcnc.STATE_ESTOP_RESET))
issue(lambda: command.state(linuxcnc.STATE_ON))
issue(lambda: command.mode(linuxcnc.MODE_MANUAL))
issue(lambda: command.teleop_enable(0))
wait(lambda: status.motion_mode == linuxcnc.TRAJ_MODE_FREE, "joint mode")

# Positive individual requests run concurrently and a third can join later.
issue(lambda: command.home(0))
wait_for_start(0)
issue(lambda: command.home(2))
wait_for_start(0, 2)
issue(lambda: command.home(1))
wait_for_start(0, 1, 2)
assert all(homing(joint) for joint in (0, 1, 2))

# A duplicate request is rejected without restarting the joint.
issue(lambda: command.home(0), linuxcnc.RCS_ERROR)
wait_error("joint 0 is already homing")
assert state(0) == HOME_UNLOCK_WAIT

# One joint can finish while the others continue and publish independent state.
finish_joint(2)
assert not homing(2)
assert homing(0) and homing(1)

# Aborting one independent home does not cancel the other.
issue(lambda: command.jog(linuxcnc.JOG_STOP, 1, 0))
wait(
    lambda: state(0) == HOME_IDLE and not homing(0) and not status.joint[0]["homed"],
    "joint 0 abort",
)
assert state(1) == HOME_UNLOCK_WAIT and homing(1)

# HOME ALL and synchronized groups cannot be mixed into independent homing.
issue(lambda: command.home(-1), linuxcnc.RCS_ERROR)
wait_error("Homing sequence not possible")
issue(lambda: command.home(3), linuxcnc.RCS_ERROR)
wait_error("Homing sequence not possible")

# A joint whose home input may be tripped by another joint (HOME_IS_SHARED
# or HOME_IGNORE_LIMITS) cannot start while another joint is homing.
for joint in (5, 6):
    issue(lambda: command.home(joint), linuxcnc.RCS_ERROR)
    wait_error("Cannot home joint %d while joint 1 is homing" % joint)
    assert state(joint) == HOME_IDLE and not homing(joint)
assert state(1) == HOME_UNLOCK_WAIT and homing(1)

finish_joint(1)

# Upstream's closed shared home switch check still applies.
component["joint-5-home-switch"] = True
issue(lambda: command.home(5))
wait(
    lambda: state(5) == HOME_IDLE and not homing(5) and not status.joint[5]["homed"],
    "joint 5 homing error cleanup",
)
wait_error("Cannot home while shared home switch is closed j=5")
component["joint-5-home-switch"] = False

# While such a joint homes, no other joint can start homing.
for joint in (5, 6):
    issue(lambda: command.home(joint))
    wait_for_start(joint)
    for other in (0, 5 if joint == 6 else 6):
        issue(lambda: command.home(other), linuxcnc.RCS_ERROR)
        wait_error("Cannot home joint %d while joint %d is homing" % (other, joint))
        assert state(other) == HOME_IDLE and not homing(other)
    finish_joint(joint)

issue(lambda: command.home(0))
wait_for_start(0)
finish_joint(0)

# A synchronized group must not publish completion until its sequence
# controller can accept the next request.  Leave joint 2 unhomed so completing
# the group does not take finish_homing_cycle()'s all-homed fast path.
issue(lambda: command.unhome(2))
wait(lambda: not status.joint[2]["homed"], "joint 2 unhome")
issue(lambda: command.home(3))
finish_synchronized_group()
issue(lambda: command.home(2))
wait_for_start(2)
finish_joint(2)

# HOME ALL retains HOME_SEQUENCE ordering and starts the synchronized group last.
issue(lambda: command.teleop_enable(0))
wait(lambda: status.motion_mode == linuxcnc.TRAJ_MODE_FREE, "joint mode for HOME ALL")
issue(lambda: command.home(-1))
wait_for_start(0)
assert all(state(joint) == HOME_IDLE for joint in (1, 2, 3, 4))
finish_joint(0, wait_for_published_completion=False)
wait_for_start(1)
assert all(state(joint) == HOME_IDLE for joint in (2, 3, 4))
finish_joint(1, wait_for_published_completion=False)
wait_for_start(2)
assert all(state(joint) == HOME_IDLE for joint in (3, 4))
finish_joint(2, wait_for_published_completion=False)
finish_synchronized_group()
wait(lambda: all(status.homed[:7]), "HOME ALL completion")
wait(
    lambda: status.motion_mode == linuxcnc.TRAJ_MODE_TELEOP,
    "automatic teleop transition",
)
x_before_jog = status.joint_position[0]
issue(lambda: command.jog(linuxcnc.JOG_INCREMENT, 0, 0, 1, 0.1))
wait(
    lambda: abs(status.joint_position[0] - x_before_jog - 0.1) < 1e-6,
    "axis jog after HOME ALL",
)

print("concurrent homing regression passed")
