#!/usr/bin/env python3
"""Exercise HALUI -> NML -> real motion, observing simulated axis positions."""
import math
import os
import sys
import time
import hal
import linuxcnc

c, s, errors = linuxcnc.command(), linuxcnc.stat(), linuxcnc.error_channel()
h = hal.component('power-jog-test')
h.ready()


def wait(predicate, label, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s.poll()
        if predicate():
            return
        time.sleep(.005)
    raise AssertionError((label, s.position, s.task_state, errors.poll()))


def done():
    assert c.wait_complete(8) == linuxcnc.RCS_DONE, errors.poll()


def pin(name, value):
    hal.set_p(name, str(value))


def velocity(axis):
    return hal.get_value('axis.' + axis + '.teleop-vel-cmd')


def position():
    s.poll()
    return s.position[0], s.position[2]


def release():
    for axis in ('x', 'z'):
        for direction in ('plus', 'minus'):
            pin('halui.power-feed.' + axis + '.' + direction, 0)
            pin('halui.axis.' + axis + '.' + direction, 0)
    wait(lambda: abs(velocity('x')) + abs(velocity('z')) < 1e-8, 'stop')
    time.sleep(.1)  # let HALUI consume the complete release


def origin():
    release()
    c.mode(linuxcnc.MODE_MDI); done()
    c.mdi('G21 G90 G94 F120 G1 X0 Z0'); done()
    c.mode(linuxcnc.MODE_MANUAL); done()
    c.teleop_enable(1); done()


wait(lambda: hal.component_exists('halui'), 'HALUI startup')
wait(lambda: any(p['NAME'] == 'halui.axis.jog-speed' for p in hal.get_info_pins()), 'HALUI pins')
if os.environ.get('POWER_JOG_NONLATHE'):
    assert not any(p['NAME'].startswith('halui.power-feed.') for p in hal.get_info_pins())
    print('nonlathe pins absent', flush=True)
    sys.exit(0)
c.state(linuxcnc.STATE_ESTOP_RESET); done()
c.state(linuxcnc.STATE_ON); done()
c.mode(linuxcnc.MODE_MANUAL); done()
c.home(-1); done()
wait(lambda: all(s.homed[:2]), 'homing')
c.teleop_enable(1); done()
c.spindle(linuxcnc.SPINDLE_FORWARD, 600, 0); done()
c.spindle(linuxcnc.SPINDLE_REVERSE, 1200, 1); done()
pin('halui.power-feed.per-rev', .1)
time.sleep(.1)

# Feed/rev is independent of the interpreter F and is captured until release.
pin('halui.power-feed.z.plus', 1)
wait(lambda: abs(velocity('z') - 1) < .01, 'initial feed')
pin('halui.power-feed.per-rev', .2)
time.sleep(.15)
assert abs(velocity('z') - 1) < .01
c.feedrate(.5)
wait(lambda: abs(velocity('z') - .5) < .01, 'live override')
c.feedrate(0)
wait(lambda: abs(velocity('z')) < 1e-8, 'zero override pauses')
c.feedrate(.5)
wait(lambda: abs(velocity('z') - .5) < .01, 'override resumes held feed')
pin('halui.power-feed.x.plus', 1)
wait(lambda: abs(velocity('x') - math.sqrt(.125)) < .01 and
     abs(velocity('z') - math.sqrt(.125)) < .01,
     'direction change retains captured feed')
release()
c.feedrate(1); done()
pin('halui.power-feed.z.plus', 1)
wait(lambda: abs(velocity('z') - 2) < .01, 'next press captures feed')
release()
pin('halui.power-feed.spindle', 1)
pin('halui.power-feed.z.minus', 1)
wait(lambda: abs(velocity('z') + 4) < .01, 'selected reverse spindle magnitude')
release()
# The spindle pin is 64 bits wide; indices that do not fit the command must
# be refused, not wrapped onto spindle 0 or 1.
for invalid in (1 << 31, 1 << 32, (1 << 32) + 1):
    pin('halui.power-feed.spindle', invalid)
    pin('halui.power-feed.z.plus', 1)
    time.sleep(.3)
    assert abs(velocity('z')) < 1e-8, ('invalid spindle fed', invalid)
    release()

# Unequal X/Z constraints must preserve geometry throughout acceleration/stop.
pin('halui.power-feed.spindle', 0)
pin('halui.power-feed.per-rev', 1)
for axis, slope in [(a, k) for a in ('x', 'z') for k in (-1, -.5, 0, .5, 1)]:
    origin()
    pin('halui.power-feed.taper-slope', slope)
    start_x, start_z = position()
    pin('halui.power-feed.' + axis + '.plus', 1)
    wait(lambda: velocity(axis) > .1, 'taper starts')
    for _ in range(35):
        x, z = position()
        error = ((x-start_x) - slope*(z-start_z) if axis == 'z'
                 else (z-start_z) + slope*(x-start_x))
        assert abs(error) < .002, (axis, slope, x, z)
        assert abs(velocity('x')) <= 3.001
        time.sleep(.005)
    pin('halui.power-feed.' + axis + '.plus', 0)
    wait(lambda: abs(velocity(axis)) < 1e-8, 'taper stops')
    x, z = position()
    error = ((x-start_x) - slope*(z-start_z) if axis == 'z'
             else (z-start_z) + slope*(x-start_x))
    assert abs(error) < .002
    release()

origin()
pin('halui.power-feed.taper-slope', 0)
pin('halui.power-feed.per-rev', .1)
pin('halui.power-feed.x.plus', 1)
pin('halui.power-feed.z.plus', 1)
wait(lambda: abs(velocity('x') - math.sqrt(.5)) < .01 and
     abs(velocity('z') - math.sqrt(.5)) < .01, 'combined directions')
c.jog(linuxcnc.JOG_STOP, 0, 0); done()
wait(lambda: abs(velocity('x')) + abs(velocity('z')) < 1e-8, 'existing stop stops both')
time.sleep(.1)
assert abs(velocity('z')) < 1e-8
release()

origin()
pin('halui.power-feed.taper-slope', .5)
pin('halui.power-feed.per-rev', 1)
pin('halui.power-feed.z.plus', 1)
wait(lambda: s.position[2] > 9.99, 'coupled travel limit', 10)
wait(lambda: abs(velocity('x')) + abs(velocity('z')) < 1e-8, 'limit stops both')
x, z = position()
assert z <= 10.000001 and abs(x - .5*z) < .002
release()

origin()
pin('halui.power-feed.z.plus', 1)
wait(lambda: velocity('z') > .1, 'jog before inhibit')
pin('motion.jog-inhibit', 1)
wait(lambda: abs(velocity('z')) < 1e-8, 'inhibit cancels')
pin('motion.jog-inhibit', 0)
time.sleep(.15)
assert abs(velocity('z')) < 1e-8
pin('halui.power-feed.x.plus', 1)
time.sleep(.15)
assert abs(velocity('x')) + abs(velocity('z')) < 1e-8
release()

# A new vector must settle before starting in its new direction.
origin()
pin('halui.power-feed.taper-slope', 0)
pin('halui.power-feed.per-rev', .1)
pin('halui.power-feed.z.plus', 1)
wait(lambda: abs(velocity('z') - 1) < .01, 'before vector change')
pin('halui.power-feed.x.plus', 1)
wait(lambda: abs(velocity('x') - math.sqrt(.5)) < .01 and
     abs(velocity('z') - math.sqrt(.5)) < .01, 'new combined vector')
pin('motion.jog-stop-immediate', 1)
wait(lambda: abs(velocity('x')) + abs(velocity('z')) < 1e-8, 'immediate group stop')
pin('motion.jog-stop-immediate', 0)
time.sleep(.15)
assert abs(velocity('x')) + abs(velocity('z')) < 1e-8
release()

# The two direction routes are independent. An ordinary jog cannot take over
# a reserved power-feed path, and a power request cannot steal an ordinary jog.
origin()
pin('halui.axis.jog-speed', 30)
pin('halui.power-feed.z.plus', 1)
wait(lambda: abs(velocity('z') - 1) < .01, 'power feed before ordinary press')
pin('halui.axis.z.minus', 1)
time.sleep(.15)
assert velocity('z') > 0
pin('halui.axis.z.minus', 0)
release()

origin()
pin('halui.axis.z.minus', 1)
wait(lambda: abs(velocity('z') + .5) < .01, 'ordinary jog before power press')
pin('halui.power-feed.z.plus', 1)
time.sleep(.15)
assert velocity('z') < 0
pin('halui.power-feed.z.plus', 0)
release()

print('native power jog scenarios ok', flush=True)
