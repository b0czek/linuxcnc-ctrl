#!/usr/bin/env python3
import time
import os
import linuxcnc
import hal
h = hal.component("availability-test")
h.ready()
c = linuxcnc.command()
s = linuxcnc.stat()
e = linuxcnc.error_channel()
def wait(fn, message, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        s.poll()
        # Expected rejection diagnostics must not fill the bounded NML error
        # channel and block task while later scenarios wait for completion.
        while e.poll() is not None:
            pass
        if fn(): return
        time.sleep(.005)
    raise AssertionError('%s: task=%s exec=%s joints=%r spindle=%r error=%r' %
                         (message, s.task_state, s.exec_state,
                          [(j['availability_state'], j['availability_fault']) for j in s.joint],
                          s.spindle[0], e.poll()))
def done():
    assert c.wait_complete(8) == linuxcnc.RCS_DONE, "command failed: %r" % (e.poll(),)
    s.poll()
def pin(name, value): hal.set_p(name, str(value))
def manual():
    c.mode(linuxcnc.MODE_MANUAL); done()
    c.teleop_enable(0); done()
def mdi(code): c.mdi(code); done()
def reset():
    c.state(linuxcnc.STATE_OFF); c.wait_complete(2)
    for n in (1, 2): pin('joint.%d.availability-ack' % n, 0)
    pin('spindle.0.availability-ack', 0)
    time.sleep(.05)
    for n in (1, 2):
        c.set_joint_availability(n, False); c.wait_complete(2)
    c.set_spindle_availability(0, False); c.wait_complete(2)
    wait(lambda: all(s.joint[n]['availability_state'] == 0 for n in (1,2)), 'release')
    pin('motion.enable', 1)
    c.state(linuxcnc.STATE_ESTOP_RESET); done()
    c.state(linuxcnc.STATE_ON); done()
    manual()
    c.home(-1); done()
    wait(lambda: s.homed[0], 'Home All homes X')
    assert all(not s.joint[n]['homing'] for n in (1,2))
    assert not s.joint[2]['homed']
    assert all(not hal.get_value('joint.%d.availability-request' % n) for n in (1,2))
wait(lambda: s.task_state == linuxcnc.STATE_ESTOP, 'startup')
reset()
# Motion can be DONE while task is dwelling with more MDI work queued.
c.mode(linuxcnc.MODE_MDI); done()
c.mdi('G4 P2')
wait(lambda: s.exec_state == linuxcnc.EXEC_WAITING_FOR_DELAY, 'active dwell')
c.mdi('G0 X0.25')
assert c.wait_complete(.05) == -1
c.mdi('G0 X0.5')
wait(lambda: s.queued_mdi_commands > 0, 'queued MDI')
for request in (lambda: c.set_joint_availability(1, True),
                lambda: c.set_spindle_availability(0, True)):
    request()
    assert c.wait_complete(.2) == linuxcnc.RCS_ERROR, 'busy direct request queued'
    assert not hal.get_value('joint.1.availability-request')
    assert not hal.get_value('spindle.0.availability-request')
wait(lambda: s.interp_state == linuxcnc.INTERP_IDLE and
     s.exec_state == linuxcnc.EXEC_DONE, 'queued work completion')
assert abs(s.joint_position[0] - .5) < .001
c.mode(linuxcnc.MODE_AUTO); done()
c.program_open(os.path.abspath('paused.ngc')); done()
c.auto(linuxcnc.AUTO_RUN, 0)
wait(lambda: s.interp_state == linuxcnc.INTERP_PAUSED, 'paused AUTO')
c.set_joint_availability(1, False)
assert c.wait_complete(.2) == linuxcnc.RCS_ERROR, 'paused AUTO accepted direct no-op'
c.auto(linuxcnc.AUTO_RESUME); done()
manual()
# All overlaps, including duplicate/no-op requests, must leave the first intact.
c.set_spindle_availability(0, True)
wait(lambda: hal.get_value('spindle.0.availability-request'), 'original spindle request')
for request in (lambda: c.set_spindle_availability(0, True),
                lambda: c.set_spindle_availability(0, False),
                lambda: c.set_joint_availability(1, True),
                lambda: c.set_joint_availability(1, False),
                lambda: c.mode(linuxcnc.MODE_MDI),
                lambda: c.jog(linuxcnc.JOG_INCREMENT, 1, 0, 1, 1)):
    request()
    assert c.wait_complete(.2) == linuxcnc.RCS_ERROR
    assert hal.get_value('spindle.0.availability-request'), 'overlap canceled owner'
    assert not hal.get_value('joint.1.availability-request')
pin('spindle.0.availability-ack', 1)
wait(lambda: s.spindle[0]['available'] and s.exec_state == linuxcnc.EXEC_DONE,
     'original request completion')
c.set_spindle_availability(0, False)
wait(lambda: not hal.get_value('spindle.0.availability-request'), 'spindle release')
pin('spindle.0.availability-ack', 0); done()
# A released spindle's amplifier may report not-ready while axes remain usable.
pin('spindle.0.amp-fault-in', 1)
c.mode(linuxcnc.MODE_MDI); done()
mdi('G0 X0.75')
assert s.enabled and abs(s.joint_position[0] - .75) < .001
manual()
c.set_spindle_availability(0, True)
wait(lambda: hal.get_value('spindle.0.availability-request'), 'faulted spindle acquire')
assert s.enabled, 'pending spindle acquisition enabled fault checking too early'
pin('spindle.0.availability-ack', 1)
wait(lambda: not s.enabled, 'reacquired spindle amplifier fault did not disable motion')
pin('spindle.0.amp-fault-in', 0)
reset()
# Deliberately released unhomed joints permit unrelated MDI and explicit no-op Y.
c.mode(linuxcnc.MODE_MDI); done()
mdi('G0 X1 Y0 Z0')
assert abs(s.joint_position[0]-1) < .01
# First prohibited actual target faults the whole coordinated operation.
c.mdi('G1 X10 Y10 F60')
wait(lambda: s.task_state != linuxcnc.STATE_ON, 'illegal Y did not fault')
assert abs(s.joint_position[1]) < 1e-9
reset()
# Direct joint jogging is rejected; teleop uses the same actual-target guard.
c.teleop_enable(0); done()
wait(lambda: s.motion_mode == linuxcnc.TRAJ_MODE_FREE, "joint mode")
c.jog(linuxcnc.JOG_INCREMENT, 1, 1, 1, 1)
assert c.wait_complete(3) == linuxcnc.RCS_ERROR
assert abs(s.joint_position[1]) < 1e-9
c.abort(); done()
c.teleop_enable(1); done()
wait(lambda: s.motion_mode == linuxcnc.TRAJ_MODE_TELEOP, "teleop mode")
c.jog(linuxcnc.JOG_INCREMENT, 0, 1, 1, 1)
wait(lambda: not s.enabled, 'illegal teleop target did not fault')
assert abs(s.joint_position[1]) < 1e-9
reset()
# Abort then late ACK cannot complete a pending acquisition.
c.set_joint_availability(1, True)
wait(lambda: s.joint[1]['availability_state'] == 1, 'pending acquisition')
c.abort(); c.wait_complete(2)
pin('joint.1.availability-ack', 1)
time.sleep(.05); s.poll()
assert not s.joint[1]['available']
assert not hal.get_value('joint.1.amp-enable-out')
# Clear the fault explicitly after hardware ACK goes low.
pin('joint.1.availability-ack', 0)
c.set_joint_availability(1, False); c.wait_complete(2)
reset()
# Enable withdrawal followed by a late ACK revokes runtime acquisition.
c.set_joint_availability(1, True)
wait(lambda: s.joint[1]['availability_state'] == 1, 'second acquisition')
pin('motion.enable', 0)
pin('joint.1.availability-ack', 1)
time.sleep(.05); s.poll()
assert not s.enabled and not s.joint[1]['available']
pin('joint.1.availability-ack', 0)
c.set_joint_availability(1, False); c.wait_complete(2)
reset()
# Establish synchronization while acquired, then clear it after release.
c.set_spindle_availability(0, True)
wait(lambda: hal.get_value('spindle.0.availability-request'), 'spindle request')
pin('spindle.0.availability-ack', 1); done()
c.mode(linuxcnc.MODE_MDI); done()
mdi('G95 F1')
manual()
c.set_spindle_availability(0, False)
wait(lambda: not hal.get_value('spindle.0.availability-request'), 'spindle release')
pin('spindle.0.availability-ack', 0); done()
c.mode(linuxcnc.MODE_MDI); done()
mdi('G94')
manual()
# An unhomed nonvolatile joint homes on first acquisition. Ordinary individual
# HOME/UNHOME commands cannot bypass availability ownership.
held = hal.get_value('joint.1.pos-cmd')
c.set_joint_availability(1, True)
wait(lambda: hal.get_value('joint.1.availability-request'), 'request Y')
pin('joint.1.availability-ack', 1); done()
wait(lambda: s.joint[1]['available'] and s.joint[1]['homed'], 'home and acquire Y')
assert abs(hal.get_value('joint.1.pos-cmd') - held) < 1e-12
c.home(1)
assert c.wait_complete(3) == linuxcnc.RCS_ERROR
c.abort(); done(); manual()
c.unhome(1)
assert c.wait_complete(3) == linuxcnc.RCS_ERROR
c.unhome(-1)
assert c.wait_complete(3) == linuxcnc.RCS_ERROR
c.abort(); done(); manual()
assert s.homed[0] and s.joint[1]['available'] and s.joint[1]['homed']
# Release freezes the logical joint and Cartesian pose. External feedback remains
# visible at HAL, but a material mismatch rejects acquisition.
held = hal.get_value('joint.1.pos-cmd')
c.set_joint_availability(1, False)
wait(lambda: not hal.get_value('joint.1.availability-request'), 'direct release')
pin('joint.1.availability-ack', 0); done()
pin('mux2.0.in1', hal.get_value('joint.1.motor-pos-fb') + 3)
pin('mux2.0.sel', 1)
time.sleep(.05); s.poll()
assert abs(hal.get_value('joint.1.motor-pos-fb') - (held + 3)) < 1e-6
assert abs(s.joint_position[1] - held) < 1e-9
assert abs(hal.get_value('joint.1.pos-fb') - held) < 1e-9
c.set_joint_availability(1, True)
wait(lambda: hal.get_value('joint.1.availability-request'), 'mismatching acquire')
pin('joint.1.availability-ack', 1)
wait(lambda: s.joint[1]['availability_fault'] == 8, 'position mismatch fault')
assert not s.joint[1]['available']
assert not hal.get_value('joint.1.amp-enable-out')
assert abs(hal.get_value('joint.1.pos-cmd') - held) < 1e-12
assert abs(s.joint_position[1] - held) < 1e-9
pin('joint.1.availability-ack', 0)
pin('mux2.0.sel', 0)
reset()
# Runtime volatile acquisition may home, but cancellation must clean it up.
pin('mux2.1.in1', 7); pin('mux2.1.sel', 1)
time.sleep(.05); s.poll()
assert abs(hal.get_value('joint.2.motor-pos-fb') - 7) < 1e-6
assert abs(s.joint_position[2]) < 1e-9
c.set_joint_availability(2, True)
wait(lambda: hal.get_value('joint.2.availability-request'), 'request Z')
pin('joint.2.availability-ack', 1)
wait(lambda: s.joint[2]['availability_state'] == 2, 'hardware ACK enters homing')
assert not s.joint[2]['available'] and not s.joint[2]['homed']
pin('mux2.1.sel', 0)
for state, switch in [(7, 1), (10, 0), (12, 1)]:
    wait(lambda: hal.get_value('joint.2.home-state') == state, 'home switch phase %d' % state)
    assert not hal.get_value('joint.2.available')
    assert c.wait_complete(.02) == -1, 'completed before required homing'
    pin('joint.2.home-sw-in', switch)
done()
wait(lambda: s.joint[2]['available'] and s.joint[2]['homed'], 'completed home')
c.set_joint_availability(2, False)
wait(lambda: not hal.get_value('joint.2.availability-request'), 'release homed Z')
pin('joint.2.availability-ack', 0); done()

# A later volatile acquisition remains unavailable until homing completes, and
# cancellation leaves it unavailable and unreferenced.
c.set_joint_availability(2, True)
wait(lambda: hal.get_value('joint.2.availability-request'), 'request Z')
pin('joint.2.availability-ack', 1)
wait(lambda: abs(hal.get_value('joint.2.vel-cmd')) > .1, 'moving runtime home Z')
c.set_joint_availability(2, False)
assert c.wait_complete(.2) == linuxcnc.RCS_ERROR, 'release canceled acquisition homing'
assert hal.get_value('joint.2.availability-request')
c.abort()
wait(lambda: not hal.get_value('joint.2.availability-request'), 'cancel request')
assert c.wait_complete(.03) == -1, 'Abort completed before release ACK/cleanup'
pin('joint.2.availability-ack', 0)
done()
wait(lambda: not s.joint[2]['homing'] and s.joint[2]['availability_state'] == 0, 'homing abort cleanup')
assert not hal.get_value('joint.2.amp-enable-out')
assert not hal.get_value('joint.2.active')
assert s.joint[2]['inpos'], 'released joint not complete'
wait(lambda: hal.get_value('joint.2.vel-cmd') == 0, 'homing velocity stopped')
wait(lambda: s.state == linuxcnc.RCS_DONE, 'normal completion after cancellation')
# Late hardware acknowledgement cannot revive cancelled homing.
pin('joint.2.availability-ack', 1)
time.sleep(.05); s.poll()
assert not s.joint[2]['available'] and not hal.get_value('joint.2.amp-enable-out')
pin('joint.2.availability-ack', 0)
reset()
# A fresh acquisition can complete homing and only then become available.
pin('joint.2.home-sw-in', 0)
c.set_joint_availability(2, True)
wait(lambda: hal.get_value('joint.2.availability-request'), 'fresh request Z')
pin('joint.2.availability-ack', 1)
for state, switch in [(7, 1), (10, 0), (12, 1)]:
    wait(lambda: hal.get_value('joint.2.home-state') == state, 'home switch phase %d' % state)
    assert not hal.get_value('joint.2.available')
    assert c.wait_complete(.02) == -1, 'completed before required homing'
    pin('joint.2.home-sw-in', switch)
done()
wait(lambda: s.joint[2]['available'] and s.joint[2]['homed'], 'completed home')
time.sleep(.05)
assert abs(hal.get_value('joint.2.free-pos-cmd') - s.joint_position[2]) < 1e-5
# Losing an acquired ACK inhibits the actuator and propagates a fault.
pin('joint.2.availability-ack', 0)
wait(lambda: s.joint[2]['availability_fault'] != 0, 'ACK loss fault')
assert not hal.get_value('joint.2.amp-enable-out')
# A release timeout is failure, even though the actuator is already inhibited.
reset()
c.set_spindle_availability(0, True)
wait(lambda: hal.get_value('spindle.0.availability-request'), 'timeout setup acquire')
pin('spindle.0.availability-ack', 1); done()
c.set_spindle_availability(0, False)
assert c.wait_complete(5) == linuxcnc.RCS_ERROR, 'release timeout reported success'
s.poll()
assert s.spindle[0]['availability_fault'] == 3
# Invalid reacquisition feedback must fail explicitly, never publish AVAILABLE.
reset()
c.set_joint_availability(1, True)
wait(lambda: hal.get_value('joint.1.availability-request'), 'invalid-feedback home setup')
pin('joint.1.availability-ack', 1); done()
assert s.joint[1]['available'] and s.joint[1]['homed']
c.set_joint_availability(1, False)
wait(lambda: not hal.get_value('joint.1.availability-request'), 'invalid-feedback release setup')
pin('joint.1.availability-ack', 0); done()
pin('mux2.0.in1', 'nan'); pin('mux2.0.sel', 1)
c.set_joint_availability(1, True)
wait(lambda: hal.get_value('joint.1.availability-request'), 'invalid-feedback request')
pin('joint.1.availability-ack', 1)
wait(lambda: s.joint[1]['availability_fault'] == 8, 'invalid-feedback position mismatch')
assert not s.joint[1]['available'] and not hal.get_value('joint.1.amp-enable-out')
print('runtime availability scenarios ok')
