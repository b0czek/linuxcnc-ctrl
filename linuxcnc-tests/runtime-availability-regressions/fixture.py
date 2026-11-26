#!/usr/bin/env python3
import os
import time
import hal
import linuxcnc

h = hal.component('availability-regression')
h.ready()
c = linuxcnc.command()
s = linuxcnc.stat()
e = linuxcnc.error_channel()
errors = []
case = os.environ.get('AVAILABILITY_REGRESSION_CASE', 'extra')

def pin(name, value):
    hal.set_p(name, str(value))

def wait(predicate, message, timeout=8):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        s.poll()
        while (error := e.poll()) is not None:
            errors.append(error)
        if predicate():
            return
        time.sleep(.005)
    raise AssertionError(f'{case}: {message}: task={s.task_state} '
                         f'interp={s.interp_state} exec={s.exec_state} errors={errors}')

def settle(seconds=.1):
    until = time.monotonic() + seconds
    wait(lambda: time.monotonic() >= until, 'settle')

def done():
    assert c.wait_complete(8) == linuxcnc.RCS_DONE, (case, errors)
    s.poll()

def acquire(resource):
    kind, number = resource.split('.')
    getattr(c, f'set_{kind}_availability')(int(number), True)
    wait(lambda: hal.get_value(resource + '.availability-request'), 'acquire ' + resource)
    pin(resource + '.availability-ack', 1)
    done()

def release(resource):
    kind, number = resource.split('.')
    getattr(c, f'set_{kind}_availability')(int(number), False)
    wait(lambda: not hal.get_value(resource + '.availability-request'), 'release ' + resource)
    pin(resource + '.availability-ack', 0)
    done()

def run(name):
    c.mode(linuxcnc.MODE_AUTO); done()
    c.program_open(os.path.abspath(name)); done()
    c.auto(linuxcnc.AUTO_RUN, 0)

wait(lambda: s.task_state == linuxcnc.STATE_ESTOP, 'startup')
if case == 'extra':
    raise AssertionError('unsupported extra-joint configuration reached UI')
c.state(linuxcnc.STATE_ESTOP_RESET); done()
c.state(linuxcnc.STATE_ON); done()
c.teleop_enable(0); done()
if case != 'fault':
    acquire('joint.0')
if case not in ('fault', 'joghome', 'slowtask'):
    acquire('joint.1')
if case == 'joghome':
    c.set_joint_availability(1, True)
    wait(lambda: hal.get_value('joint.1.availability-request'), 'runtime acquisition')
    pin('joint.1.availability-ack', 1)
    wait(lambda: hal.get_value('joint.1.index-enable'), 'runtime homing index')
    pin('joint.1.index-enable', 0)
    done()
if case == 'slowtask':
    # an unhomed joint is homed when acquired at runtime
    c.set_joint_availability(1, True)
    wait(lambda: hal.get_value('joint.1.availability-request'), 'runtime acquisition')
    pin('joint.1.availability-ack', 1)
    done()
c.home(2); done()
wait(lambda: s.homed[2], 'home ordinary joint')
