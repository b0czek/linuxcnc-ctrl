#!/usr/bin/env python3

import math
import os
import subprocess
import sys
import time

import hal
import linuxcnc


ORIENT_IN_PROGRESS = 2


def fail(message):
    print(message)
    sys.exit(1)


def wait_for(predicate, message, timeout=10.0, poll_interval=0.02):
    start = time.time()
    while time.time() - start < timeout:
        s.poll()
        if predicate():
            return
        time.sleep(poll_interval)
    if callable(message):
        message = message()
    fail(
        "%s: X=%.6f Z=%.6f vel=%.6f queue=%d task_paused=%s "
        "motion_paused=%s stepping=%s "
        "interp=%d exec=%d"
        % (
            message,
            s.position[0],
            s.position[2],
            s.current_vel,
            s.queue,
            s.task_paused,
            s.paused,
            s.single_stepping,
            s.interp_state,
            s.exec_state,
        )
    )


def wait_complete(message, timeout=10.0):
    if c.wait_complete(timeout) != linuxcnc.RCS_DONE:
        fail(message)
    s.poll()


c = linuxcnc.command()
s = linuxcnc.stat()
e = linuxcnc.error_channel()
comp = hal.component("resumable-stop-test")

wait_for(lambda: s.task_state == linuxcnc.STATE_ESTOP, "LinuxCNC did not initialize")

c.state(linuxcnc.STATE_ESTOP_RESET)
c.state(linuxcnc.STATE_ON)
wait_complete("machine did not turn on")


def open_auto_program(path):
    c.mode(linuxcnc.MODE_AUTO)
    wait_complete("machine did not enter AUTO")
    c.program_open(path)
    wait_complete("%s did not open" % path)


# Simulated motion runs slower than real time on a loaded machine, so allow
# well beyond a program's nominal run time; a passing run never waits longer.
def wait_for_program_complete(expected_x, expected_z=0.0, timeout=60.0):
    wait_for(
        lambda: s.interp_state == linuxcnc.INTERP_IDLE
        and s.exec_state == linuxcnc.EXEC_DONE
        and s.queue == 0
        and math.isclose(s.position[0], expected_x, abs_tol=0.01)
        and math.isclose(s.position[2], expected_z, abs_tol=0.01),
        "program did not complete at expected position",
        timeout=timeout,
    )


def assert_outputs_stopped_after_program():
    if abs(hal.get_value("spindle.0.speed-out")) > 0.001:
        fail("spindle was not stopped by program completion")

    if hal.get_value("iocontrol.0.coolant-flood"):
        fail("flood coolant was not stopped by program completion")


def run_idle_program_stop_rejected_test():
    c.mode(linuxcnc.MODE_AUTO)
    wait_complete("machine did not enter AUTO")

    c.program_stop()
    if c.wait_complete(10.0) != linuxcnc.RCS_ERROR:
        fail("program Stop was not rejected without an active AUTO program")

    s.poll()
    if s.stop_state != linuxcnc.STOP_IDLE:
        fail("rejected program Stop changed Stop state")


def resume_and_wait_for_x_motion(stopped_x, on_poll=None, on_resume=None):
    c.auto(linuxcnc.AUTO_RESUME)
    if on_resume:
        on_resume()

    saw_restored_spindle = False
    saw_restored_coolant = False
    start = time.time()
    while time.time() - start < 10.0:
        s.poll()
        if on_poll:
            on_poll()
        if hal.get_value("spindle.0.speed-out") > 0.0:
            saw_restored_spindle = True
        if hal.get_value("iocontrol.0.coolant-flood"):
            saw_restored_coolant = True
        if s.position[0] > stopped_x + 0.05:
            break
        time.sleep(0.02)

    if not saw_restored_spindle:
        fail("resume did not restore spindle")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > stopped_x + 0.05,
        "program did not resume motion from stopped position",
    )

    return saw_restored_coolant


def spin_up_after_restore(message):
    # at-speed has read false with the spindle on earlier, so after a Stop
    # restore task waits for at-speed to drop and come back, as a real
    # spindle reports it while spinning up again.
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STARTING
        and hal.get_value("spindle.0.on"),
        "%s: Resume did not restore the spindle and wait for at-speed" % message,
    )
    hal.set_p("spindle.0.at-speed", "false")
    time.sleep(0.05)
    hal.set_p("spindle.0.at-speed", "true")


def run_ordinary_stop_resume_test():
    open_auto_program("test.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.task_mode == linuxcnc.MODE_AUTO
        and s.interp_state != linuxcnc.INTERP_IDLE
        and s.queue > 0
        and s.position[0] > 0.05
        and s.position[0] < 9.0
        and hal.get_value("spindle.0.speed-out") > 0.0
        and hal.get_value("iocontrol.0.coolant-flood"),
        "program did not reach active motion with spindle and coolant on",
    )

    line_before_stop = s.read_line
    queue_before_stop = s.queue

    c.program_stop()
    # Spindle and coolant are switched off on entry to STOPPED, so it must not
    # be reported while the feed is still decelerating.
    start = time.time()
    while time.time() - start < 10.0:
        s.poll()
        if s.stop_state == linuxcnc.STOP_STOPPED:
            if abs(s.current_vel) > 1e-9:
                fail("STOPPED reported while still moving at %.6f" % s.current_vel)
            break
        time.sleep(0.001)
    wait_complete("stop command did not complete")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED state",
    )

    stopped_x = s.position[0]
    stopped_read_line = s.read_line
    stopped_queue = s.queue

    time.sleep(0.25)
    s.poll()

    if abs(s.position[0] - stopped_x) > 0.001:
        fail("position changed while stopped: %.6f -> %.6f" % (stopped_x, s.position[0]))

    if stopped_read_line < line_before_stop:
        fail("read line regressed across stop: %d -> %d" % (line_before_stop, stopped_read_line))

    if stopped_queue <= 0 or stopped_queue > queue_before_stop:
        fail("queue was not preserved across stop: before=%d stopped=%d" % (queue_before_stop, stopped_queue))

    if abs(hal.get_value("spindle.0.speed-out")) > 0.001:
        fail("spindle speed-out was not disabled while stopped")

    if hal.get_value("iocontrol.0.coolant-flood"):
        fail("flood coolant was not disabled while stopped")

    if not resume_and_wait_for_x_motion(stopped_x):
        fail("resume did not restore coolant")

    wait_for_program_complete(10.0)
    assert_outputs_stopped_after_program()


def run_stop_single_step_test():
    open_auto_program("single-step.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.queue > 0
        and s.position[0] > 0.05
        and s.position[0] < 9.0
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "program did not reach active motion before single-step test",
    )

    c.program_stop()
    wait_complete("single-step stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED before single-step",
    )

    stopped_x = s.position[0]
    c.auto(linuxcnc.AUTO_STEP)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.task_paused
        and s.position[0] > stopped_x + 0.05
        and abs(s.current_vel) < 0.001,
        "single-step did not execute one queued motion and pause",
        timeout=15.0,
    )

    stepped_x = s.position[0]
    time.sleep(0.25)
    s.poll()
    if abs(s.position[0] - stepped_x) > 0.001:
        fail("program continued after stop-to-single-step")

    if abs(hal.get_value("spindle.0.speed-out")) < 0.001:
        fail("single-step did not restore spindle")
    if not hal.get_value("iocontrol.0.coolant-flood"):
        fail("single-step did not restore coolant")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(20.0)
    assert_outputs_stopped_after_program()

def run_inflight_single_step_test():
    open_auto_program("inflight-step.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.queue >= 3
        and s.position[0] > 1.0
        and s.position[0] < 9.0,
        "program did not prequeue later blocks during its first move",
    )

    c.auto(linuxcnc.AUTO_STEP)
    wait_for(
        lambda: s.single_stepping and not s.task_paused and not s.paused,
        "in-flight Single Step did not remain unpaused while armed",
    )
    wait_for(
        lambda: s.task_paused
        and s.paused
        and math.isclose(s.position[0], 10.0, abs_tol=0.01)
        and abs(s.current_vel) < 0.001,
        "in-flight Single Step did not finish the active source block",
        timeout=15.0,
    )

    if s.queue < 3:
        fail("in-flight Single Step did not preserve prequeued later blocks")

    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[0], 10.0, abs_tol=0.001):
        fail("in-flight Single Step advanced into the next source block")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(40.0, timeout=10.0)


def run_inflight_short_block_step_test():
    # Step armed at full speed among blocks shorter than the stopping
    # distance must stop on the first block end it can reach within the
    # acceleration limit, not cut the speed at the end of the current block.
    # Runs with the trapezoidal planner: the S-curve planner already limits
    # the speed in short blocks to one each block can stop in.
    open_auto_program("inflight-step-short.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: 10.0 < s.position[0] < 13.0 and s.current_vel > 45.0,
        "program did not reach full speed in its short blocks",
        timeout=15.0,
        poll_interval=0.001,
    )
    hal.set_p("accel-peak.reset", "1")
    time.sleep(0.01)
    hal.set_p("accel-peak.reset", "0")
    armed_x = s.position[0]
    c.auto(linuxcnc.AUTO_STEP)
    wait_for(
        lambda: s.task_paused and s.paused and abs(s.current_vel) < 0.001,
        "in-flight Single Step did not stop",
        timeout=15.0,
    )
    stopped_x = s.position[0]
    peak_decel = -hal.get_value("accel-peak.min")
    print(
        "short-block Step armed at X=%.4f, stopped at X=%.4f, peak "
        "deceleration %.1f" % (armed_x, stopped_x, peak_decel)
    )
    if peak_decel > 500.0 * 1.05:
        fail(
            "in-flight Single Step decelerated at %.1f, above the 500 limit"
            % peak_decel
        )
    block_end = round(stopped_x * 2.0) / 2.0
    if not math.isclose(stopped_x, block_end, abs_tol=0.001) or not (
        10.5 <= block_end <= 20.0
    ):
        fail(
            "in-flight Single Step stopped at X=%.4f, not on a short block end"
            % stopped_x
        )
    if block_end <= armed_x:
        fail("in-flight Single Step stopped behind where it was armed")

    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[0], stopped_x, abs_tol=0.001):
        fail("in-flight Single Step advanced past its block end")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(40.0, timeout=10.0)


def run_pause_inflight_single_step_test():
    # Pause during an armed Single Step is an ordinary Pause: motion stops
    # where it is instead of finishing the block, and Resume continues.
    open_auto_program("inflight-step.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.queue >= 3 and 1.0 < s.position[0] < 5.0,
        "program did not prequeue later blocks during its first move",
    )
    c.auto(linuxcnc.AUTO_STEP)
    wait_for(
        lambda: s.single_stepping and not s.task_paused and not s.paused,
        "in-flight Single Step did not remain unpaused while armed",
    )
    c.auto(linuxcnc.AUTO_PAUSE)
    wait_for(
        lambda: s.task_paused and s.paused and abs(s.current_vel) < 0.001,
        "Pause did not stop the in-flight Single Step",
        timeout=15.0,
    )
    paused_x = s.position[0]
    if paused_x > 9.5:
        fail("Pause let the Single Step finish its block: X=%.3f" % paused_x)
    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[0], paused_x, abs_tol=0.001):
        fail("motion continued after Pause during Single Step")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(40.0, timeout=10.0)


def run_rapid_resume_without_at_speed_test():
    hal.set_p("spindle.0.at-speed", "false")
    open_auto_program("rapid-then-feed.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.task_mode == linuxcnc.MODE_AUTO
        and s.position[0] > 1.0
        and s.position[0] < 70.0
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "program did not enter active rapid with spindle at-speed false",
    )

    c.program_stop()
    wait_complete("rapid stop command did not complete")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not stop during active rapid",
    )

    stopped_x = s.position[0]
    c.auto(linuxcnc.AUTO_RESUME)

    # Resume waits for the restored spindle to report at-speed before any
    # motion continues, also an interrupted rapid.
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STARTING
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "Resume did not restore the spindle and wait for at-speed",
    )
    time.sleep(0.25)
    s.poll()
    if s.stop_state != linuxcnc.STOP_STARTING or \
            abs(s.position[0] - stopped_x) > 0.001:
        fail("rapid resumed before the restored spindle was at speed")
    hal.set_p("spindle.0.at-speed", "true")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > stopped_x + 0.05,
        "rapid did not resume once the spindle was at speed",
    )
    # The program's own M3 barrier still holds the following feed.
    hal.set_p("spindle.0.at-speed", "false")

    wait_for(
        lambda: math.isclose(s.position[0], 80.0, abs_tol=0.05)
        and abs(s.current_vel) < 0.001
        and s.queue > 0,
        "rapid did not complete and wait at following feed with at-speed false",
    )

    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[0], 80.0, abs_tol=0.05):
        fail("following feed moved before spindle at-speed became true")

    hal.set_p("spindle.0.at-speed", "true")
    wait_for_program_complete(90.0)
    assert_outputs_stopped_after_program()


def run_stop_while_program_paused_test(restart_with_run):
    open_auto_program("pause.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.interp_state == linuxcnc.INTERP_PAUSED
        and s.task_paused
        and math.isclose(s.position[0], 5.0, abs_tol=0.01),
        "program did not reach M0 pause",
    )

    c.program_stop()
    wait_complete("stop command did not complete from M0 pause")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED from M0 pause",
    )

    if restart_with_run:
        c.auto(linuxcnc.AUTO_RUN, 0)
    else:
        c.auto(linuxcnc.AUTO_RESUME)

    wait_for_program_complete(10.0)


def run_abort_from_empty_stopped_queue_test():
    open_auto_program("pause.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.interp_state == linuxcnc.INTERP_PAUSED
        and s.task_paused
        and math.isclose(s.position[0], 5.0, abs_tol=0.01),
        "program did not reach M0 pause before abort test",
    )

    c.program_stop()
    wait_complete("stop command did not complete from M0 pause before abort")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED from M0 pause before abort",
    )

    c.abort()
    wait_complete("abort command did not complete from empty stopped queue")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and not s.task_paused
        and s.interp_state == linuxcnc.INTERP_IDLE
        and s.exec_state == linuxcnc.EXEC_DONE
        and s.queue == 0,
        "abort did not clear empty STOPPED state",
    )

    open_auto_program("after-abort.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.position[0] > 5.05,
        "motion did not start after aborting empty STOPPED queue",
    )

    wait_for_program_complete(6.0)


def stop_during_sync_motion(program, active_predicate, complete_predicate, message, expected_z,
                            resume_max_vel=None):
    open_auto_program(program)

    c.auto(linuxcnc.AUTO_RUN, 0)

    start = time.time()
    while time.time() - start < 15.0:
        s.poll()
        if (
            s.task_mode == linuxcnc.MODE_AUTO
            and hal.get_value("spindle.0.speed-out") > 0.0
            and active_predicate()
        ):
            break
        time.sleep(0.02)
    else:
        err = e.poll()
        fail(
            "%s did not enter synchronized motion: X=%.6f Z=%.6f "
            "interp=%d exec=%d queue=%d spindle=%.3f error=%s"
            % (
                message,
                s.position[0],
                s.position[2],
                s.interp_state,
                s.exec_state,
                s.queue,
                hal.get_value("spindle.0.speed-out"),
                err,
            )
        )

    c.program_stop()
    wait_complete("%s stop command did not complete" % message)

    start = time.time()
    while time.time() - start < 15.0:
        s.poll()
        if s.stop_state == linuxcnc.STOP_STOPPED:
            break
        time.sleep(0.02)

    if s.stop_state != linuxcnc.STOP_STOPPED:
        fail("%s did not enter STOPPED state" % message)

    if not complete_predicate():
        fail(
            "%s STOPPED before synchronized motion endpoint/cycle completion: "
            "X=%.6f Z=%.6f"
            % (message, s.position[0], s.position[2])
        )

    # With the trapezoidal planner a blend arc joins the synchronized move
    # to the following X move; Stop finishes it, which moves X a little.
    if s.position[0] > 0.1:
        fail("%s allowed following X move to start before STOPPED: X=%.6f" % (message, s.position[0]))

    if abs(hal.get_value("spindle.0.speed-out")) > 0.001:
        fail("%s spindle speed-out was not disabled while stopped" % message)

    stopped_x = s.position[0]
    stopped_z = s.position[2]
    peak = [0.0]

    def check_resume():
        # Nothing synchronized is left after Stop: a lunge to catch up with
        # the spindle shows as Z motion or as speed above the next feed.
        peak[0] = max(peak[0], abs(s.current_vel))
        if abs(s.position[2] - stopped_z) > 0.01:
            fail("%s moved Z after Resume: %.4f -> %.4f at %.3f"
                 % (message, stopped_z, s.position[2], peak[0]))

    if resume_max_vel is None:
        resume_and_wait_for_x_motion(stopped_x)
    else:
        resume_and_wait_for_x_motion(stopped_x, check_resume)
        start = time.time()
        while time.time() - start < 15.0:
            s.poll()
            check_resume()
            if s.interp_state == linuxcnc.INTERP_IDLE and s.queue == 0:
                break
            time.sleep(0.002)
        if peak[0] > resume_max_vel:
            fail("%s moved at %.3f after Resume, above %.3f"
                 % (message, peak[0], resume_max_vel))
    wait_for_program_complete(10.0, expected_z=expected_z)
    assert_outputs_stopped_after_program()


def run_threading_stop_resume_test():
    stop_during_sync_motion(
        "thread.ngc",
        lambda: s.position[2] < -0.5 and s.position[2] > -4.5,
        lambda: s.position[2] <= -4.90,
        "G33 threading",
        expected_z=-5.0,
    )


def run_g33_does_not_execute_following_rapid_test():
    open_auto_program("g33-followed-by-rapid.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.position[2] < -0.5 and s.position[2] > -4.5,
        "G33 did not enter synchronized motion before negative Stop test",
        timeout=15.0,
    )
    c.program_stop()
    wait_complete("G33 negative Stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "G33 negative test did not reach STOPPED",
        timeout=15.0,
    )

    if s.position[2] > -4.90:
        fail("G33 negative test stopped before its synchronized move completed")
    if not math.isclose(s.position[0], 10.0, abs_tol=0.05):
        fail(
            "Stop executed the unmarked rapid following G33: X=%.6f"
            % s.position[0]
        )

    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[0], 10.0, abs_tol=0.05):
        fail("unmarked post-G33 rapid moved while stopped")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(30.0, expected_z=-5.0, timeout=20.0)
    assert_outputs_stopped_after_program()


def run_fperrev_stop_resume_test():
    open_auto_program("fperrev.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.task_mode == linuxcnc.MODE_AUTO
        and s.position[0] > 0.5
        and s.position[0] < 9.0
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "G95 G1 did not enter feed-per-revolution motion",
        timeout=15.0,
    )

    c.program_stop()
    wait_complete("G95 G1 stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not stop during G95 G1 motion",
    )

    stopped_x = s.position[0]
    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[0], stopped_x, abs_tol=0.001):
        fail("G95 G1 moved while stopped")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > stopped_x + 0.05
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "G95 G1 did not resume with the spindle active",
    )

    wait_for_program_complete(10.0)
    assert_outputs_stopped_after_program()


def run_g76_stop_resume_test():
    open_auto_program("g76.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.task_mode == linuxcnc.MODE_AUTO
        and s.position[0] < 9.5
        and s.position[2] < -0.5
        and s.position[2] > -4.5,
        "G76 did not enter its first synchronized pass",
        timeout=15.0,
    )

    c.program_stop()
    wait_complete("G76 stop command did not complete")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not stop after the active G76 threading TC",
        timeout=15.0,
    )

    if s.position[2] > -4.90:
        fail("G76 stopped before its active threading TC completed")

    if not math.isclose(s.position[0], 10.4, abs_tol=0.05):
        fail(
            "G76 did not stop at the first pass clearance: X=%.6f"
            % s.position[0]
        )

    stopped_x = s.position[0]
    stopped_z = s.position[2]
    time.sleep(0.25)
    s.poll()
    if (
        not math.isclose(s.position[0], stopped_x, abs_tol=0.001)
        or not math.isclose(s.position[2], stopped_z, abs_tol=0.001)
    ):
        fail("G76 moved while stopped")

    resume_and_wait_for_x_motion(stopped_x)
    wait_for_program_complete(20.0, expected_z=-5.0)
    assert_outputs_stopped_after_program()


def run_g76_stop_while_waiting_for_index_test():
    subprocess.run(["halcmd", "unlinkp", "encoder_0.phase-Z"], check=True)
    hal.set_p("encoder_0.phase-Z", "false")
    open_auto_program("g76.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: hal.get_value("spindle.0.index-enable")
        and s.position[0] < 9.95
        and math.isclose(s.position[2], 0.0, abs_tol=0.001)
        and abs(s.current_vel) < 0.001,
        "G76 did not arm its first cut while waiting for spindle index",
        timeout=15.0,
    )

    c.program_stop()
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "G76 did not stop with its index-waiting cut preserved",
    )

    # Deliver the delayed index after Stop. The armed cut must remain queued
    # for Resume rather than starting merely because its prerequisite arrived.
    subprocess.run(
        ["halcmd", "net", "spindle-phase-Z", "encoder_0.phase-Z"],
        check=True,
    )

    if not math.isclose(s.position[2], 0.0, abs_tol=0.001):
        fail("G76 started its cut after Stop: Z=%.6f" % s.position[2])

    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[2], 0.0, abs_tol=0.001):
        fail("index-waiting G76 moved while stopped: Z=%.6f" % s.position[2])

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(20.0, expected_z=-5.0, timeout=40.0)
    assert_outputs_stopped_after_program()


def run_tapered_g76_stop_resume_test():
    open_auto_program("g76-tapered.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.task_mode == linuxcnc.MODE_AUTO
        and s.position[0] < 8.6
        and s.position[2] < -0.5
        and s.position[2] > -4.5,
        "tapered G76 did not enter its first synchronized pass",
        timeout=15.0,
    )

    c.program_stop()
    wait_complete("tapered G76 stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not stop after the active tapered G76 pass",
        timeout=15.0,
    )

    if s.position[2] > -4.90:
        fail("tapered G76 stopped before its synchronized cut completed")
    if not math.isclose(s.position[0], 8.4, abs_tol=0.05):
        fail(
            "tapered G76 did not stop at the first pass clearance: X=%.6f Z=%.6f"
            % (s.position[0], s.position[2])
        )

    stopped_x = s.position[0]
    stopped_z = s.position[2]
    time.sleep(0.25)
    s.poll()
    if (
        not math.isclose(s.position[0], stopped_x, abs_tol=0.001)
        or not math.isclose(s.position[2], stopped_z, abs_tol=0.001)
    ):
        fail("tapered G76 moved while stopped")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(20.0, expected_z=-5.0)
    assert_outputs_stopped_after_program()


def step_g76_approach(expected_approach_x, label="G76"):
    c.auto(linuxcnc.AUTO_STEP)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.task_paused
        and abs(s.current_vel) < 0.001
        and math.isclose(s.position[2], 0.0, abs_tol=0.05)
        and math.isclose(s.position[0], expected_approach_x, abs_tol=0.05),
        "%s Single Step did not stop at the approach endpoint X%.3f"
        % (label, expected_approach_x),
        timeout=20.0,
    )

    stepped_x = s.position[0]
    stepped_z = s.position[2]
    time.sleep(0.25)
    s.poll()
    if (
        not math.isclose(s.position[0], stepped_x, abs_tol=0.001)
        or not math.isclose(s.position[2], stepped_z, abs_tol=0.001)
    ):
        fail("%s advanced into its synchronized cut" % label)


def step_g76_thread_and_clearance(expected_clear_x, label="G76"):
    c.auto(linuxcnc.AUTO_STEP)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.task_paused
        and abs(s.current_vel) < 0.001
        and s.position[2] <= -4.90
        and math.isclose(s.position[0], expected_clear_x, abs_tol=0.05),
        "%s Single Step did not finish the thread and clearance at X%.3f"
        % (label, expected_clear_x),
        timeout=20.0,
    )

    stepped_x = s.position[0]
    stepped_z = s.position[2]
    time.sleep(0.25)
    s.poll()
    if (
        not math.isclose(s.position[0], stepped_x, abs_tol=0.001)
        or not math.isclose(s.position[2], stepped_z, abs_tol=0.001)
    ):
        fail("%s advanced into another pass after the thread Step" % label)


def run_g76_single_step_program(program, expected_steps, label):
    c.mode(linuxcnc.MODE_MDI)
    wait_complete("G76 Single Step setup did not enter MDI")
    c.mdi("G21 G18 G90 G0 X10 Z0")
    wait_complete("G76 Single Step setup did not reach X10 Z0")
    c.spindle(linuxcnc.SPINDLE_FORWARD, 60.0, 0)
    wait_complete("G76 Single Step setup did not start the spindle")

    open_auto_program(program)

    # LinuxCNC's first AUTO_STEP from IDLE primes the program in PAUSED state.
    c.auto(linuxcnc.AUTO_STEP)
    wait_for(
        lambda: s.interp_state == linuxcnc.INTERP_PAUSED and s.task_paused,
        "G76 Single Step did not prime the program",
    )

    for expected_approach, expected_clearance in expected_steps:
        step_g76_approach(expected_approach, label)
        step_g76_thread_and_clearance(expected_clearance, label)

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(20.0, expected_z=-5.0)
    assert_outputs_stopped_after_program()


def run_g76_single_step_test():
    run_g76_single_step_program(
        "g76-step.ngc", ((10.4, 10.4), (10.2, 10.2), (10.0, 10.0)),
        "cylindrical G76",
    )


def run_tapered_g76_single_step_test():
    run_g76_single_step_program(
        "g76-tapered-step.ngc",
        ((10.4, 8.4), (10.2, 8.2), (10.0, 8.0)),
        "tapered-drive G76",
    )

def run_inflight_g76_single_step_test():
    open_auto_program("g76.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.queue > 3
        and s.position[0] < 9.5
        and s.position[2] < -0.5
        and s.position[2] > -4.5,
        "G76 did not prequeue later passes during its first cut",
        timeout=15.0,
    )

    c.auto(linuxcnc.AUTO_STEP)
    wait_for(
        lambda: s.single_stepping and not s.task_paused and not s.paused,
        "in-flight G76 Single Step did not remain unpaused while armed",
    )
    wait_for(
        lambda: s.task_paused
        and s.paused
        and abs(s.current_vel) < 0.001
        and s.position[2] <= -4.90
        and math.isclose(s.position[0], 10.4, abs_tol=0.01),
        "in-flight G76 Single Step did not finish the active thread and clearance",
        timeout=20.0,
    )

    stepped_x = s.position[0]
    stepped_z = s.position[2]
    time.sleep(0.25)
    s.poll()
    if (
        not math.isclose(s.position[0], stepped_x, abs_tol=0.001)
        or not math.isclose(s.position[2], stepped_z, abs_tol=0.001)
    ):
        fail("in-flight G76 Single Step advanced into the next approach")

    # Later passes are still prequeued. The next Step must end exactly at the
    # pass 2 approach point (no blend into the infeed may trim it), and the
    # one after it must run the pass 2 infeed, thread and clearance.
    step_g76_approach(10.2, "prequeued G76")
    if not (
        math.isclose(s.position[0], 10.2, abs_tol=0.001)
        and math.isclose(s.position[2], 0.0, abs_tol=0.001)
    ):
        fail(
            "prequeued G76 Single Step stopped at X=%.6f Z=%.6f, not exactly "
            "at the approach point X10.2 Z0" % (s.position[0], s.position[2])
        )
    step_g76_thread_and_clearance(10.2, "prequeued G76")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(20.0, expected_z=-5.0, timeout=20.0)
    assert_outputs_stopped_after_program()


def run_g76_exit_taper_stop_test(program, expected_clear_x, label):
    open_auto_program(program)
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.position[2] < -0.5
        and s.position[2] > -3.5
        and s.position[0] < 9.5,
        "%s did not enter its first synchronized pass" % label,
        timeout=15.0,
    )
    c.program_stop()
    wait_complete("%s Stop command did not complete" % label)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "%s did not reach STOPPED" % label,
        timeout=15.0,
    )

    if s.position[2] > -4.90 or not math.isclose(
        s.position[0], expected_clear_x, abs_tol=0.05
    ):
        fail(
            "%s did not complete its synchronized exit taper and clearance: "
            "X=%.6f Z=%.6f" % (label, s.position[0], s.position[2])
        )

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(20.0, expected_z=-5.0)
    assert_outputs_stopped_after_program()


def run_cylindrical_g76_exit_taper_stop_test():
    run_g76_exit_taper_stop_test(
        "g76-exit-taper.ngc", 10.4, "cylindrical exit-taper G76"
    )


def run_tapered_g76_exit_taper_stop_test():
    run_g76_exit_taper_stop_test(
        "g76-tapered-exit-taper.ngc", 8.4, "tapered-drive exit-taper G76"
    )


def run_g76_cut_retract_handoff_stress_test():
    # At S60 and P1 the synchronized feed is 1 mm/s. Even the closest
    # trigger remains 20 task cycles before the endpoint, so Python polling
    # reliably submits Stop while the cut is active instead of after retract.
    thresholds = (-0.92, -0.94, -0.96, -0.97, -0.98) * 2

    for attempt, threshold in enumerate(thresholds, 1):
        open_auto_program("g76-handoff.ngc")
        c.auto(linuxcnc.AUTO_RUN, 0)
        wait_for(
            lambda: s.position[2] <= threshold and s.position[0] < 9.5,
            "G76 handoff attempt %d missed the cut/retract boundary" % attempt,
            timeout=10.0,
            poll_interval=0.001,
        )

        c.program_stop()
        wait_complete(
            "G76 handoff attempt %d Stop command did not complete" % attempt
        )
        wait_for(
            lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
            "G76 handoff attempt %d did not reach STOPPED" % attempt,
            timeout=10.0,
            poll_interval=0.001,
        )

        if s.position[2] > -0.99 or not math.isclose(
            s.position[0], 10.01, abs_tol=0.01
        ):
            fail(
                "G76 handoff attempt %d missed complete clearance: "
                "X=%.6f Z=%.6f" % (attempt, s.position[0], s.position[2])
            )

        c.auto(linuxcnc.AUTO_RESUME)
        wait_for_program_complete(20.0, expected_z=-1.0, timeout=10.0)
        assert_outputs_stopped_after_program()


def run_queued_threading_stop_resume_test():
    hal.set_p("spindle.0.at-speed", "false")
    open_auto_program("thread.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.task_mode == linuxcnc.MODE_AUTO
        and s.interp_state != linuxcnc.INTERP_IDLE
        and s.queue > 0
        and math.isclose(s.position[0], 0.0, abs_tol=0.001)
        and math.isclose(s.position[2], 0.0, abs_tol=0.001)
        and abs(s.current_vel) < 0.001
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "G33 did not queue while waiting for spindle at-speed",
    )

    # Let the stationary queue state persist so the G0 before G33 is gone.
    time.sleep(0.25)
    s.poll()
    if s.queue <= 0 or abs(s.current_vel) >= 0.001:
        fail("G33 did not remain queued while spindle at-speed was false")

    c.program_stop()
    wait_complete("queued G33 stop command did not complete")

    # Simulate the delayed prerequisite arriving after Stop. The queued G33
    # must remain inactive and preserved for Resume.
    time.sleep(0.05)
    hal.set_p("spindle.0.at-speed", "true")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not stop with an inactive G33 queued",
    )

    if not math.isclose(s.position[2], 0.0, abs_tol=0.001):
        fail("queued G33 started after Stop: Z=%.6f" % s.position[2])

    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[2], 0.0, abs_tol=0.001):
        fail("queued G33 moved while stopped: Z=%.6f" % s.position[2])

    resume_and_wait_for_x_motion(
        s.position[0],
        on_resume=lambda: spin_up_after_restore("queued G33"))
    wait_for_program_complete(10.0, expected_z=-5.0)
    assert_outputs_stopped_after_program()


def run_rigid_tap_stop_resume_test():
    saw_downstroke = False

    def active_predicate():
        nonlocal saw_downstroke
        if s.position[2] < -0.5:
            saw_downstroke = True
            return True
        return False

    def complete_predicate():
        return saw_downstroke and s.position[2] >= -0.05

    stop_during_sync_motion(
        "tap.ngc",
        active_predicate,
        complete_predicate,
        "G33.1 rigid tapping",
        expected_z=0.0,
    )


def run_spindle_orientation_stop_test():
    hal.set_p("spindle.0.is-oriented", "false")
    open_auto_program("orient.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: hal.get_value("spindle.0.orient")
        and s.spindle[0]["orient_state"] == ORIENT_IN_PROGRESS,
        "M19 did not start spindle orientation",
    )

    x_before_stop = s.position[0]
    c.program_stop()
    wait_complete("M19 stop command did not complete")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED during M19",
    )

    if hal.get_value("spindle.0.orient"):
        fail("task reported STOPPED while spindle orientation remained active")

    if s.spindle[0]["orient_state"] == ORIENT_IN_PROGRESS:
        fail("task reported STOPPED while spindle orientation was in progress")

    if not math.isclose(s.position[0], x_before_stop, abs_tol=0.001):
        fail("following motion started while stopping M19")

    c.auto(linuxcnc.AUTO_RESUME)

    wait_for(
        lambda: hal.get_value("spindle.0.orient")
        and s.spindle[0]["orient_state"] == ORIENT_IN_PROGRESS,
        "resume did not restore spindle orientation",
    )

    if not math.isclose(
        hal.get_value("spindle.0.orient-angle"), 90.0, abs_tol=0.001
    ):
        fail("resume did not restore the requested spindle orientation angle")

    if hal.get_value("spindle.0.orient-mode") != 1:
        fail("resume did not restore the requested spindle orientation mode")

    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[0], x_before_stop, abs_tol=0.001):
        fail("following motion started before spindle orientation completed")

    hal.set_p("spindle.0.is-oriented", "true")

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > x_before_stop + 0.05,
        "program did not continue after spindle orientation completed",
    )

    wait_for_program_complete(20.0, timeout=25.0)


def run_dwell_stop_resume_test():
    open_auto_program("dwell.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.exec_state == linuxcnc.EXEC_WAITING_FOR_DELAY
        and s.delay_left > 2.0,
        "G4 did not enter its dwell",
    )

    c.program_stop()
    wait_complete("G4 stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED during G4",
    )

    stopped_delay = s.delay_left
    time.sleep(0.5)
    s.poll()
    if not math.isclose(s.position[0], 0.0, abs_tol=0.001):
        fail("motion started while G4 was stopped")
    if s.delay_left < stopped_delay - 0.1:
        fail("G4 delay elapsed while stopped")

    c.auto(linuxcnc.AUTO_RESUME)
    time.sleep(0.5)
    s.poll()
    if not math.isclose(s.position[0], 0.0, abs_tol=0.001):
        fail("motion started before the restored G4 delay elapsed")

    wait_for_program_complete(5.0)


def run_dwell_atspeed_barrier_test():
    # M3 arms motion's next-feed at-speed barrier before the dwell, and the
    # following feed is not queued until the dwell ends. Turning the spindle
    # off and on around Stop must leave that barrier armed.
    hal.set_p("spindle.0.at-speed", "false")
    open_auto_program("dwell-atspeed.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.exec_state == linuxcnc.EXEC_WAITING_FOR_DELAY
        and s.delay_left > 1.0
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "G4 did not enter its dwell with the spindle running",
    )

    c.program_stop()
    wait_complete("G4 stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED during G4",
    )
    if abs(hal.get_value("spindle.0.speed-out")) > 0.001:
        fail("spindle was not stopped while STOPPED")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STARTING
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "Resume did not restore the spindle and wait for at-speed",
    )
    # Resume waits for at-speed; once it is reached, the spindle drops out of
    # speed again before the dwell ends, and the barrier armed by M3 must
    # still hold the feed.
    hal.set_p("spindle.0.at-speed", "true")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE,
        "program did not resume once the spindle was at speed",
    )
    hal.set_p("spindle.0.at-speed", "false")
    s.poll()
    if s.exec_state != linuxcnc.EXEC_WAITING_FOR_DELAY:
        fail("G4 ended before the test could drop at-speed again")
    wait_for(
        lambda: s.exec_state != linuxcnc.EXEC_WAITING_FOR_DELAY
        and s.queue > 0,
        "feed after G4 was not queued after Resume",
    )

    time.sleep(0.5)
    s.poll()
    if hal.get_value("spindle.0.speed-out") <= 0.0:
        fail("Resume did not restore the spindle")
    if not math.isclose(s.position[0], 0.0, abs_tol=0.001):
        fail("feed after Stop ignored the at-speed barrier armed by M3")

    hal.set_p("spindle.0.at-speed", "true")
    wait_for_program_complete(5.0)
    assert_outputs_stopped_after_program()


def run_command_after_sync_motion_test():
    # Stop lets a G33 or rigid tap finish. The program's next command (M5,
    # an S-word or M9) must not run while Stop is still completing: it must
    # take effect once, after Resume, in program order.
    cases = (
        ("after-thread-m5.ngc", lambda: s.position[2] < -1.0 and s.position[2] > -8.0,
         lambda: not hal.get_value("spindle.0.on"), "M5 after G33"),
        ("after-thread-s.ngc", lambda: s.position[2] < -1.0 and s.position[2] > -8.0,
         lambda: hal.get_value("spindle.0.on")
         and math.isclose(hal.get_value("spindle.0.speed-out"), 500.0, abs_tol=1.0),
         "S500 after G33"),
        ("after-thread-m9.ngc", lambda: s.position[2] < -1.0 and s.position[2] > -8.0,
         lambda: not hal.get_value("iocontrol.0.coolant-flood"), "M9 after G33"),
        ("after-tap-m5.ngc", lambda: s.position[2] < -1.0,
         lambda: not hal.get_value("spindle.0.on"), "M5 after G33.1"),
    )
    for program, during_sync, expected_after, name in cases:
        open_auto_program(program)
        c.auto(linuxcnc.AUTO_RUN, 0)
        wait_for(
            lambda: hal.get_value("spindle.0.speed-out") > 0.0 and during_sync(),
            "%s: synchronized motion did not start" % name,
            timeout=15.0,
        )
        c.program_stop()
        wait_for(
            lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
            "%s: task did not enter STOPPED" % name,
            timeout=15.0,
        )
        if s.position[0] > 0.1:
            fail("%s: the move after the command started before STOPPED" % name)
        if hal.get_value("spindle.0.on"):
            fail("%s: spindle on while STOPPED" % name)

        c.auto(linuxcnc.AUTO_RESUME)
        wait_for(
            lambda: s.stop_state == linuxcnc.STOP_IDLE and s.position[0] > 0.5,
            "%s: program did not resume to the following move" % name,
        )
        s.poll()
        if not expected_after():
            fail(
                "%s: outputs after Resume do not match the program: "
                "spindle.on=%d speed-out=%.1f flood=%d"
                % (name, hal.get_value("spindle.0.on"),
                   hal.get_value("spindle.0.speed-out"),
                   hal.get_value("iocontrol.0.coolant-flood"))
            )
        wait_for_program_complete(5.0, expected_z=s.position[2], timeout=15.0)


def stop_feed_at(min_x, message):
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and hal.get_value("spindle.0.on")
        and s.current_vel > 0.0
        and min_x < s.position[0] < 3.5,
        "%s: feed did not reach X%.1f" % (message, min_x),
    )
    c.program_stop()
    wait_complete("%s: Stop command did not complete" % message)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "%s: task did not enter STOPPED" % message,
    )
    if hal.get_value("spindle.0.on"):
        fail("%s: spindle on while STOPPED" % message)
    return s.position[0]


def assert_held_in_starting(stopped_x, message, hold=0.4):
    # The spindle is back on, but motion must not continue before at-speed.
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STARTING
        and hal.get_value("spindle.0.on")
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "%s: Resume did not restore the spindle and wait in STARTING" % message,
    )
    time.sleep(hold)
    s.poll()
    if s.stop_state != linuxcnc.STOP_STARTING:
        fail("%s: left STARTING before spindle at-speed" % message)
    if abs(s.position[0] - stopped_x) > 0.001:
        fail("%s: feed moved before spindle at-speed: %.6f -> %.6f"
             % (message, stopped_x, s.position[0]))


def wait_resumed_after_atspeed(stopped_x, message):
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > stopped_x + 0.05,
        "%s: feed did not continue once the spindle was at speed" % message,
    )


def run_resume_atspeed_wait_test():
    # Resume restarts an interrupted feed only once the restored spindle
    # reports at-speed again, and an at-speed left over from the stopped
    # spindle does not count.
    open_auto_program("resume-atspeed.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    # 1. at-speed has never read false with the spindle on (tied true, as in
    # a simulation or a machine without feedback): Resume does not wait.
    stopped_x = stop_feed_at(0.3, "tied at-speed")
    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > stopped_x + 0.05,
        "tied at-speed: Resume waited although at-speed never dropped",
        timeout=3.0,
    )

    # 2. at-speed false while stopped (a speed-reached output): the feed
    # waits in STARTING until it goes true.
    stopped_x = stop_feed_at(stopped_x + 0.3, "at-speed false while stopped")
    hal.set_p("spindle.0.at-speed", "false")
    c.auto(linuxcnc.AUTO_RESUME)
    assert_held_in_starting(stopped_x, "at-speed false while stopped")
    hal.set_p("spindle.0.at-speed", "true")
    wait_resumed_after_atspeed(stopped_x, "at-speed false while stopped")

    # 3. at-speed true throughout the Stop (a stopped spindle reported as at
    # speed, or a VFD that reports late): the stale true does not release
    # the feed; at-speed must drop after the restore and come back.
    stopped_x = stop_feed_at(stopped_x + 0.3, "stale at-speed")
    c.auto(linuxcnc.AUTO_RESUME)
    assert_held_in_starting(stopped_x, "stale at-speed")
    hal.set_p("spindle.0.at-speed", "false")
    assert_held_in_starting(stopped_x, "stale at-speed, then false", hold=0.2)
    hal.set_p("spindle.0.at-speed", "true")
    wait_resumed_after_atspeed(stopped_x, "stale at-speed")

    # 4. A second Stop while waiting leaves STARTING for STOPPED with the
    # spindle off; Single Step from there waits for at-speed again.
    stopped_x = stop_feed_at(stopped_x + 0.3, "Stop while waiting")
    c.auto(linuxcnc.AUTO_RESUME)
    assert_held_in_starting(stopped_x, "Stop while waiting")
    c.program_stop()
    wait_complete("Stop while waiting: Stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED
        and not hal.get_value("spindle.0.on"),
        "Stop while waiting: second Stop did not return to STOPPED",
    )
    c.auto(linuxcnc.AUTO_STEP)
    assert_held_in_starting(stopped_x, "Step after Stop while waiting")
    hal.set_p("spindle.0.at-speed", "false")
    time.sleep(0.05)
    hal.set_p("spindle.0.at-speed", "true")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and math.isclose(s.position[0], 4.0, abs_tol=0.001)
        and abs(s.current_vel) < 1e-9
        and s.task_paused,
        "Step after Stop while waiting: feed did not step to its end",
    )
    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(4.0)
    assert_outputs_stopped_after_program()

    # 5. Abort while waiting ends the program cleanly.
    open_auto_program("resume-atspeed.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    stopped_x = stop_feed_at(0.3, "Abort while waiting")
    c.auto(linuxcnc.AUTO_RESUME)
    assert_held_in_starting(stopped_x, "Abort while waiting")
    c.abort()
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.interp_state == linuxcnc.INTERP_IDLE
        and s.queue == 0
        and not hal.get_value("spindle.0.on"),
        "Abort while waiting did not end the program",
    )
    time.sleep(0.25)
    s.poll()
    if abs(s.position[0] - stopped_x) > 0.001:
        fail("Abort while waiting: the feed moved")


def run_pause_while_starting_test():
    # Pause while Resume waits in STARTING for spindle at-speed cancels the
    # Resume: the program returns to STOPPED with the restored spindle off,
    # and at-speed arriving later does not release the queued feed.
    open_auto_program("resume-atspeed.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    stopped_x = stop_feed_at(0.3, "Pause while starting")
    hal.set_p("spindle.0.at-speed", "false")
    c.auto(linuxcnc.AUTO_RESUME)
    assert_held_in_starting(stopped_x, "Pause while starting")
    c.auto(linuxcnc.AUTO_PAUSE)
    wait_complete("Pause while starting: Pause did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED
        and s.task_paused
        and not hal.get_value("spindle.0.on"),
        "Pause while starting: Pause did not return to STOPPED",
    )
    hal.set_p("spindle.0.at-speed", "true")
    time.sleep(0.4)
    s.poll()
    if s.stop_state != linuxcnc.STOP_STOPPED or not s.task_paused:
        fail("Pause while starting: left STOPPED once at-speed arrived")
    if hal.get_value("spindle.0.on"):
        fail("Pause while starting: spindle switched back on")
    if abs(s.position[0] - stopped_x) > 0.001:
        fail("Pause while starting: feed moved while paused: %.6f -> %.6f"
             % (stopped_x, s.position[0]))

    c.auto(linuxcnc.AUTO_RESUME)
    spin_up_after_restore("Resume after Pause while starting")
    wait_resumed_after_atspeed(stopped_x, "Resume after Pause while starting")
    if s.task_paused or s.interp_state == linuxcnc.INTERP_PAUSED:
        fail("Resume after Pause while starting: task still paused")
    wait_for_program_complete(4.0)
    assert_outputs_stopped_after_program()


def run_cancel_latched_resume_test(label, cancel):
    # Resume during STOPPING is latched until Stop completes; a Pause or a
    # second Stop after it cancels the latched Resume, so the program stays
    # STOPPED.
    open_auto_program("thread-then-steps.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.position[2] < -0.5
        and s.position[2] > -3.0
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "G33 did not start",
        timeout=15.0,
    )
    c.program_stop()
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPING,
        "Stop did not wait for the active G33",
        timeout=1.0,
    )
    c.auto(linuxcnc.AUTO_RESUME)
    cancel()
    s.poll()
    if s.stop_state != linuxcnc.STOP_STOPPING:
        fail("G33 finished before Resume and %s were sent" % label)

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED
        and math.isclose(s.position[2], -5.0, abs_tol=0.01),
        lambda: "%s after latched Resume did not leave the program "
        "STOPPED: X=%.3f Z=%.3f stop=%d"
        % (label, s.position[0], s.position[2], s.stop_state),
        timeout=15.0,
    )
    time.sleep(0.4)
    s.poll()
    if s.stop_state != linuxcnc.STOP_STOPPED or not s.task_paused:
        fail("%s after latched Resume: program left STOPPED" % label)
    if hal.get_value("spindle.0.on"):
        fail("%s after latched Resume: spindle on while STOPPED" % label)
    if abs(s.position[0]) > 0.001:
        fail("%s after latched Resume: feed after G33 moved" % label)

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.interp_state == linuxcnc.INTERP_IDLE
        and s.exec_state == linuxcnc.EXEC_DONE
        and math.isclose(s.position[0], 10.0, abs_tol=0.01),
        "program did not run to completion after Resume",
        timeout=20.0,
    )
    assert_outputs_stopped_after_program()


def run_resume_zero_speed_test():
    # A spindle restored at S0 does not spin up, so Resume does not wait for
    # an at-speed comparator that never drops for it.
    open_auto_program("resume-s0.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: hal.get_value("spindle.0.on")
        and s.current_vel > 0.0
        and 0.05 < s.position[0] < 0.2,
        "S0: feed did not start",
    )
    # at-speed has read false with the spindle on, as from a comparator at a
    # speed change, so task knows it is connected.
    hal.set_p("spindle.0.at-speed", "false")
    time.sleep(0.05)
    hal.set_p("spindle.0.at-speed", "true")
    stopped_x = stop_feed_at(0.3, "S0")
    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > stopped_x + 0.05,
        "S0: Resume waited for at-speed of a spindle restored at S0",
        timeout=3.0,
    )
    wait_for_program_complete(4.0)


def run_multi_spindle_stop_resume_test():
    open_auto_program("multi-spindle.ngc")
    c.spindle(linuxcnc.SPINDLE_FORWARD, 120.0, 0)
    wait_complete("primary spindle command did not complete")
    c.spindle(linuxcnc.SPINDLE_REVERSE, 180.0, 1)
    wait_complete("secondary spindle command did not complete")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.position[0] > 0.05
        and s.position[0] < 9.0
        and s.spindle[0]["direction"] == 1
        and s.spindle[1]["direction"] == -1
        and hal.get_value("spindle.0.on")
        and hal.get_value("spindle.1.on"),
        "both spindles did not remain active during motion",
    )

    c.program_stop()
    wait_complete("multi-spindle stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED with two spindles",
    )
    if hal.get_value("spindle.0.on") or hal.get_value("spindle.1.on"):
        fail("a spindle remained enabled while stopped")

    stopped_x = s.position[0]
    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > stopped_x + 0.05
        and s.spindle[0]["direction"] == 1
        and s.spindle[1]["direction"] == -1
        and hal.get_value("spindle.0.on")
        and hal.get_value("spindle.1.on"),
        "resume did not restore both spindles",
    )

    wait_for_program_complete(10.0)
    if hal.get_value("spindle.0.on") or hal.get_value("spindle.1.on"):
        fail("a spindle remained enabled after program completion")


def run_css_stop_resume_test():
    open_auto_program("css.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.position[0] < 18.0
        and s.position[0] > 12.0
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "G96 did not enter active CSS motion",
    )
    speed_before_stop = hal.get_value("spindle.0.speed-out")

    c.program_stop()
    wait_complete("G96 stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED during G96",
    )
    if abs(hal.get_value("spindle.0.speed-out")) > 0.001:
        fail("CSS spindle remained active while stopped")

    stopped_x = s.position[0]
    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] < stopped_x - 0.05
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "G96 motion did not resume with the spindle active",
    )
    wait_for(
        lambda: hal.get_value("spindle.0.speed-out") > speed_before_stop * 1.02,
        "G96 spindle speed did not continue tracking diameter after Resume",
    )

    wait_for_program_complete(10.0)
    assert_outputs_stopped_after_program()


EXEC_WAITING_FOR_HOMING = getattr(linuxcnc, "EXEC_WAITING_FOR_HOMING", 12)


def run_homing_stop_resume_test():
    open_auto_program("homing.ngc")

    c.auto(linuxcnc.AUTO_RUN, 0)

    wait_for(
        lambda: s.exec_state == EXEC_WAITING_FOR_HOMING
        and s.joint[0]["homing"],
        "program did not enter the queued G28.2 homing cycle",
    )

    c.program_stop()

    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPING,
        "Stop was not accepted during a queued G28.2 homing cycle",
        timeout=1.0,
    )
    if not s.joint[0]["homing"]:
        fail("G28.2 homing finished before Stop could be observed in STOPPING")

    wait_complete("Stop during G28.2 did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED after the G28.2 homing cycle",
    )

    if not s.homed[0] or s.joint[0]["homing"]:
        fail("Stop entered STOPPED before the G28.2 homing cycle completed")
    if not math.isclose(s.position[0], 20.0, abs_tol=0.01):
        fail("G28.2 final move did not finish at HOME before STOPPED")
    if s.task_mode != linuxcnc.MODE_AUTO:
        fail("task did not return to AUTO after the G28.2 mode dip")

    time.sleep(0.25)
    s.poll()
    if not math.isclose(s.position[0], 20.0, abs_tol=0.01):
        fail("program continued after G28.2 while STOPPED")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for_program_complete(30.0)


def wait_for_queued_homing(message, program="homing.ngc"):
    open_auto_program(program)
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.exec_state == EXEC_WAITING_FOR_HOMING
        and s.joint[0]["homing"],
        message,
    )


def check_stop_reaches_stopped(message):
    open_auto_program("test.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.interp_state != linuxcnc.INTERP_IDLE
        and s.position[0] > 0.05
        and s.position[0] < 9.0
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "%s: program did not reach active motion" % message,
    )
    c.program_stop()
    wait_complete("%s: Stop command did not complete" % message)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "%s: Stop did not reach STOPPED" % message,
        timeout=5.0,
    )
    c.abort()
    wait_complete("%s: abort did not complete" % message)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.interp_state == linuxcnc.INTERP_IDLE,
        "%s: abort did not clear STOPPED" % message,
    )


def run_interrupted_homing_test():
    # Each way of leaving a queued G28.2 early must leave nothing behind that
    # a later Stop depends on.
    def interrupt_abort():
        c.abort()
        wait_complete("abort during G28.2 did not complete")

    def interrupt_estop():
        c.state(linuxcnc.STATE_ESTOP)
        wait_complete("E-stop during G28.2 did not complete")
        c.state(linuxcnc.STATE_ESTOP_RESET)
        c.state(linuxcnc.STATE_ON)
        wait_complete("machine did not turn on after E-stop")

    def interrupt_off():
        c.state(linuxcnc.STATE_OFF)
        wait_complete("machine off during G28.2 did not complete")
        c.state(linuxcnc.STATE_ON)
        wait_complete("machine did not turn on after machine off")

    def interrupt_mode():
        c.mode(linuxcnc.MODE_MDI)
        wait_complete("MDI mode switch during G28.2 did not complete")

    def interrupt_stop_abort():
        # Abort while Stop waits for G28.2 reaches motion in FREE mode.
        # homing-long.ngc gives an 8 s final homing move to do this in.
        c.program_stop()
        wait_for(
            lambda: s.stop_state == linuxcnc.STOP_STOPPING,
            "Stop was not accepted during G28.2",
            timeout=5.0,
        )
        if not s.joint[0]["homing"]:
            fail("G28.2 finished before Stop and Abort could be sent")
        c.abort()
        c.mode(linuxcnc.MODE_AUTO)
        wait_complete("abort and AUTO during G28.2 did not complete")
        wait_for(
            lambda: s.task_mode == linuxcnc.MODE_AUTO
            and s.exec_state == linuxcnc.EXEC_DONE
            and not s.joint[0]["homing"],
            "abort and AUTO did not end the queued G28.2",
        )
        # Run without another mode command: its abort would hide a stale
        # Stop in motion.
        c.program_open("after-abort.ngc")
        wait_complete("after-abort.ngc did not open")
        c.auto(linuxcnc.AUTO_RUN, 0)
        wait_for_program_complete(6.0)

    for name, interrupt in (
        ("abort", interrupt_abort),
        ("estop", interrupt_estop),
        ("off", interrupt_off),
        ("mode", interrupt_mode),
        ("stop and abort", interrupt_stop_abort),
    ):
        wait_for_queued_homing(
            "program did not enter G28.2 before %s" % name,
            "homing-long.ngc" if name == "stop and abort" else "homing.ngc",
        )
        interrupt()
        wait_for(
            lambda: s.exec_state == linuxcnc.EXEC_DONE
            and s.interp_state == linuxcnc.INTERP_IDLE
            and not s.joint[0]["homing"],
            "%s did not end the queued G28.2" % name,
        )

        if name == "abort":
            # An MDI command is not an AUTO program, whatever mode the
            # interrupted G28.2 was issued from.
            c.mode(linuxcnc.MODE_MDI)
            wait_complete("machine did not enter MDI")
            c.mdi("G1 X0 F60")
            wait_for(
                lambda: abs(s.current_vel) > 0.1,
                "MDI move did not start",
            )
            c.program_stop()
            if c.wait_complete(10.0) != linuxcnc.RCS_ERROR:
                fail("Stop was accepted for an MDI command after G28.2 was aborted")
            s.poll()
            if s.stop_state != linuxcnc.STOP_IDLE:
                fail("rejected MDI Stop changed Stop state")
            c.abort()
            wait_complete("abort of MDI move did not complete")

        check_stop_reaches_stopped("after %s during G28.2" % name)


def run_speed_change_atspeed_test():
    # An S-word after Stop and Resume must still make the next feed wait for
    # spindle at-speed, also after back-to-back Stops.
    open_auto_program("speed-change.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.position[0] > 0.3
        and s.position[0] < 1.5
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "first feed did not start",
    )

    for _ in range(2):
        c.program_stop()
        wait_complete("Stop command did not complete")
        wait_for(
            lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
            "task did not enter STOPPED",
        )
        stopped_x = s.position[0]
        c.auto(linuxcnc.AUTO_RESUME)
        wait_for(
            lambda: s.stop_state == linuxcnc.STOP_IDLE
            and s.position[0] > stopped_x + 0.3,
            "program did not resume",
        )

    hal.set_p("spindle.0.at-speed", "false")
    wait_for(
        lambda: s.position[0] > 3.99,
        "first feed did not finish",
    )
    time.sleep(0.5)
    s.poll()
    if hal.get_value("spindle.0.speed-out") < 590.0:
        fail("S600 was not applied")
    if not math.isclose(s.position[0], 4.0, abs_tol=0.01):
        fail(
            "feed after S600 did not wait for spindle at-speed after Stop: "
            "X=%.6f" % s.position[0]
        )

    hal.set_p("spindle.0.at-speed", "true")
    wait_for_program_complete(6.0)
    assert_outputs_stopped_after_program()


def run_queued_feed_atspeed_test():
    # A feed already queued and held for spindle at-speed when Stop arrives
    # must still wait for at-speed after Resume.
    hal.set_p("spindle.0.at-speed", "false")
    open_auto_program("feed-atspeed.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.queue > 0
        and math.isclose(s.position[0], 0.0, abs_tol=0.001)
        and abs(s.current_vel) < 0.001
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "feed did not queue while waiting for spindle at-speed",
    )
    time.sleep(0.25)

    c.program_stop()
    wait_complete("Stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED with a feed waiting for at-speed",
    )

    # at-speed reports the stopped spindle while STOPPED, then drops again
    # when Resume restarts the spindle.
    hal.set_p("spindle.0.at-speed", "true")
    time.sleep(0.25)
    hal.set_p("spindle.0.at-speed", "false")
    s.poll()
    if not math.isclose(s.position[0], 0.0, abs_tol=0.001):
        fail("feed moved while STOPPED")

    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.stop_state != linuxcnc.STOP_STOPPED
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "Resume did not restore the spindle",
    )
    time.sleep(0.5)
    s.poll()
    if not math.isclose(s.position[0], 0.0, abs_tol=0.001):
        fail("feed started after Resume without waiting for spindle at-speed")

    hal.set_p("spindle.0.at-speed", "true")
    wait_for_program_complete(5.0)
    assert_outputs_stopped_after_program()


def run_threading_chain_stop_resume_test():
    # Consecutive G33 moves stay synchronized to one index pulse, so Stop
    # must let the whole chain finish, not just the move active at Stop.
    stop_during_sync_motion(
        "thread-chain.ngc",
        lambda: s.position[2] < -0.5 and s.position[2] > -1.5,
        lambda: s.position[2] <= -4.90,
        "consecutive G33 threading",
        expected_z=-5.0,
        resume_max_vel=1.05,
    )


def run_step_then_resume_while_stopping_test():
    # Step followed by Resume while Stop waits for G33 must resume the
    # program normally rather than single-step it.
    open_auto_program("thread-then-steps.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.position[2] < -0.5
        and s.position[2] > -3.0
        and hal.get_value("spindle.0.speed-out") > 0.0,
        "G33 did not start",
        timeout=15.0,
    )

    # Stop completes only on STOPPED, so do not wait for it here.
    c.program_stop()
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPING,
        "Stop did not wait for the active G33",
        timeout=1.0,
    )
    c.auto(linuxcnc.AUTO_STEP)
    c.auto(linuxcnc.AUTO_RESUME)
    s.poll()
    if s.stop_state != linuxcnc.STOP_STOPPING:
        fail("G33 finished before Step and Resume were sent")

    wait_for(
        lambda: s.interp_state == linuxcnc.INTERP_IDLE
        and s.exec_state == linuxcnc.EXEC_DONE
        and math.isclose(s.position[0], 10.0, abs_tol=0.01),
        lambda: "program did not run to completion after Step and Resume: "
        "X=%.3f interp=%d paused=%d stop=%d"
        % (s.position[0], s.interp_state, s.task_paused, s.stop_state),
        timeout=20.0,
    )
    assert_outputs_stopped_after_program()


def run_g95_between_g33_stop_resume_test():
    # A G33 that follows velocity-synchronized G95 motion keeps the earlier
    # G33's spindle synchronization. After a Stop during the G95 move the
    # spindle has been stopped and restarted, so the next G33 must wait for
    # a fresh index pulse instead of chasing the revolutions turned since.
    open_auto_program("thread-g95-thread.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.position[0] > 0.2
        and s.position[0] < 0.7
        and math.isclose(s.position[2], -2.0, abs_tol=0.01),
        "G95 move between the G33 moves did not start",
        timeout=15.0,
    )

    c.program_stop()
    wait_complete("Stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED during the G95 move",
    )
    if s.position[0] >= 0.99 or not math.isclose(s.position[2], -2.0, abs_tol=0.01):
        fail("Stop did not hold the G95 move: X=%.4f Z=%.4f" % (s.position[0], s.position[2]))

    c.auto(linuxcnc.AUTO_RESUME)
    saw_index_wait = False
    start = time.time()
    while time.time() - start < 15.0:
        s.poll()
        if s.position[2] < -2.05:
            break
        if s.position[0] > 0.99 and hal.get_value("spindle.0.index-enable"):
            saw_index_wait = True
        time.sleep(0.005)
    else:
        fail("second G33 did not start after Resume")
    if not saw_index_wait:
        fail("second G33 did not wait for a fresh spindle index after Stop")

    wait_for_program_complete(10.0, expected_z=-4.0, timeout=20.0)
    assert_outputs_stopped_after_program()


def run_stopped_spindle_s_word_test():
    # An S-word for a spindle stopped with M5 must leave it stopped after a
    # Stop and Resume of another spindle.
    open_auto_program("spindle-state.ngc")
    c.auto(linuxcnc.AUTO_RUN, 0)
    wait_for(
        lambda: s.position[0] > 0.5
        and s.position[0] < 2.5
        and hal.get_value("spindle.0.on"),
        "feed with spindle 0 on did not start",
    )
    c.program_stop()
    wait_complete("Stop command did not complete")
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_STOPPED and s.task_paused,
        "task did not enter STOPPED",
    )
    stopped_x = s.position[0]
    c.auto(linuxcnc.AUTO_RESUME)
    wait_for(
        lambda: s.stop_state == linuxcnc.STOP_IDLE
        and s.position[0] > stopped_x + 0.05,
        "program did not resume",
    )
    wait_for(
        lambda: s.position[0] > 4.3 and s.position[0] < 7.0,
        "feed after S800 $1 did not start",
    )
    if not hal.get_value("spindle.0.on"):
        fail("spindle 0 was not restored")
    if hal.get_value("spindle.1.on"):
        fail("S800 $1 switched on spindle 1, which M5 had stopped")
    wait_for_program_complete(8.0)


SCENARIOS = {
    "idle-rejected": run_idle_program_stop_rejected_test,
    "ordinary": run_ordinary_stop_resume_test,
    "single-step": run_stop_single_step_test,
    "inflight-single-step": run_inflight_single_step_test,
    "rapid-no-atspeed": run_rapid_resume_without_at_speed_test,
    "paused-resume": lambda: run_stop_while_program_paused_test(False),
    "paused-run": lambda: run_stop_while_program_paused_test(True),
    "abort-empty": run_abort_from_empty_stopped_queue_test,
    "queued-g33": run_queued_threading_stop_resume_test,
    "g33": run_threading_stop_resume_test,
    "g33-no-following-rapid": run_g33_does_not_execute_following_rapid_test,
    "fperrev": run_fperrev_stop_resume_test,
    "g76": run_g76_stop_resume_test,
    "g76-waiting-index": run_g76_stop_while_waiting_for_index_test,
    "g76-tapered": run_tapered_g76_stop_resume_test,
    "g76-single-step": run_g76_single_step_test,
    "g76-tapered-single-step": run_tapered_g76_single_step_test,
    "g76-inflight-single-step": run_inflight_g76_single_step_test,
    "g76-exit-taper": run_cylindrical_g76_exit_taper_stop_test,
    "g76-tapered-exit-taper": run_tapered_g76_exit_taper_stop_test,
    "g76-handoff-stress": run_g76_cut_retract_handoff_stress_test,
    "rigid-tap": run_rigid_tap_stop_resume_test,
    "orientation": run_spindle_orientation_stop_test,
    "dwell": run_dwell_stop_resume_test,
    "dwell-atspeed": run_dwell_atspeed_barrier_test,
    "command-after-sync": run_command_after_sync_motion_test,
    "resume-atspeed": run_resume_atspeed_wait_test,
    "pause-starting": run_pause_while_starting_test,
    "pause-latched-resume": lambda: run_cancel_latched_resume_test(
        "Pause", lambda: c.auto(linuxcnc.AUTO_PAUSE)),
    "stop-latched-resume": lambda: run_cancel_latched_resume_test(
        "Stop", c.program_stop),
    "pause-inflight-step": run_pause_inflight_single_step_test,
    "inflight-step-short": run_inflight_short_block_step_test,
    "resume-s0": run_resume_zero_speed_test,
    "multi-spindle": run_multi_spindle_stop_resume_test,
    "css": run_css_stop_resume_test,
    "homing": run_homing_stop_resume_test,
    "homing-interrupted": run_interrupted_homing_test,
    "speed-change-atspeed": run_speed_change_atspeed_test,
    "queued-feed-atspeed": run_queued_feed_atspeed_test,
    "g33-chain": run_threading_chain_stop_resume_test,
    "step-resume-stopping": run_step_then_resume_while_stopping_test,
    "g95-between-g33": run_g95_between_g33_stop_resume_test,
    "spindle-state": run_stopped_spindle_s_word_test,
}

scenario = os.environ.get("RESUMABLE_STOP_SCENARIO")
if scenario not in SCENARIOS:
    fail("unknown resumable-stop scenario: %s" % scenario)

SCENARIOS[scenario]()
print("resumable stop scenario ok: %s" % scenario)
sys.exit(0)
