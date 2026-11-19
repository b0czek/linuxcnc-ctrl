#!/usr/bin/env python3
import time
import linuxcnc
import hal
h = hal.component('initial-availability-test'); h.ready()
c = linuxcnc.command(); s = linuxcnc.stat()
def wait(fn, msg):
    end = time.monotonic() + 8
    while time.monotonic() < end:
        s.poll()
        if fn(): return
        time.sleep(.005)
    raise AssertionError(msg)
def pin(n, v): hal.set_p(n, str(v))
wait(lambda: s.task_state == linuxcnc.STATE_ESTOP, 'startup')
c.state(linuxcnc.STATE_ESTOP_RESET); c.wait_complete(3)
c.state(linuxcnc.STATE_ON)
wait(lambda: hal.get_value('spindle.0.availability-request'), 'initial request')
pin('motion.enable', 0)
pin('spindle.0.availability-ack', 1)
time.sleep(.05); s.poll()
assert not s.enabled and not hal.get_value('flipflop.0.out')
assert not hal.get_value('spindle.0.available')
pin('spindle.0.availability-ack', 0)
c.state(linuxcnc.STATE_OFF); c.wait_complete(3)
c.set_spindle_availability(0, False); c.wait_complete(3)
pin('motion.enable', 1)
c.state(linuxcnc.STATE_ON)
wait(lambda: hal.get_value('spindle.0.availability-request'), 'retry request')
c.abort()
wait(lambda: not hal.get_value('spindle.0.availability-request'), 'abort request')
pin('spindle.0.availability-ack', 1)
time.sleep(.05); s.poll()
assert not s.enabled and not hal.get_value('flipflop.0.out')
pin('spindle.0.availability-ack', 0)
c.state(linuxcnc.STATE_OFF); c.wait_complete(3)
c.set_spindle_availability(0, False); c.wait_complete(3)
c.state(linuxcnc.STATE_ON)
wait(lambda: hal.get_value('spindle.0.availability-request'), 'initial spindle request')
# Repeated On while the initial scan is already pending must not restart it.
c.state(linuxcnc.STATE_ON)
time.sleep(.02)
assert hal.get_value('spindle.0.availability-request')
pin('spindle.0.availability-ack', 1)
assert c.wait_complete(5) == linuxcnc.RCS_DONE
wait(lambda: s.enabled, 'machine on')
assert not s.joint[1]['available'] and not s.joint[2]['available']
# Initial availability is startup policy, not a permanent ownership requirement.
c.set_spindle_availability(0, False)
wait(lambda: not hal.get_value('spindle.0.availability-request'), 'release initial spindle')
pin('spindle.0.availability-ack', 0)
assert c.wait_complete(5) == linuxcnc.RCS_DONE
c.state(linuxcnc.STATE_ON)
assert c.wait_complete(5) == linuxcnc.RCS_DONE
time.sleep(.1); s.poll()
assert s.enabled and not s.spindle[0]['available']
assert not hal.get_value('spindle.0.availability-request')
print('initial availability scenarios ok')
