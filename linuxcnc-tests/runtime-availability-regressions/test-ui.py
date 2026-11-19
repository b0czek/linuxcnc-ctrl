#!/usr/bin/env python3
from fixture import *
from pathlib import Path

if case == 'power':
    release('joint.1')
    pin('mux2.0.in1', 180); pin('mux2.0.sel', 1)
    wait(lambda: abs(hal.get_value('joint.1.motor-pos-fb') - 180) < 1e-6,
         'live released C HAL feedback')
    settle()
    assert abs(s.actual_position[5]) < 1e-9
    assert abs(s.joint_position[1]) < 1e-9
    c.state(linuxcnc.STATE_OFF)
    wait(lambda: not hal.get_value('joint.0.availability-request'), 'Machine Off release')
    pin('joint.0.availability-ack', 0)
    done(); settle()
    assert abs(s.position[5]) < 1e-9, s.position
    assert abs(hal.get_value('joint.1.pos-cmd')) < 1e-9
    c.state(linuxcnc.STATE_ON); done()
    acquire('joint.0')
    c.mode(linuxcnc.MODE_MDI); done()
    c.mdi('G0 X2 Z3'); done()
    assert abs(s.position[0] - 2) < 1e-6 and abs(s.position[2] - 3) < 1e-6
    assert abs(s.position[5]) < 1e-9
    assert abs(s.actual_position[5]) < 1e-9

elif case == 'index':
    acquire('spindle.0')
    c.mode(linuxcnc.MODE_MDI); done()
    c.mdi('M3 S60'); done()
    c.mdi('G33 Z1 K1')
    wait(lambda: hal.get_value('spindle.0.index-enable'), 'first index wait')
    c.abort(); done(); settle()
    c.mode(linuxcnc.MODE_MANUAL); done()
    release('spindle.0')
    assert not hal.get_value('spindle.0.index-enable'), 'index latch survived release'
    assert not hal.get_value('spindle.0.on')
    assert hal.get_value('spindle.0.speed-out') == 0
    pin('spindle.0.index-enable', 1)
    settle()
    assert not hal.get_value('spindle.0.index-enable'), 'index latch reasserted while released'
    acquire('spindle.0')
    assert not hal.get_value('spindle.0.index-enable')
    c.mode(linuxcnc.MODE_MDI); done()
    c.mdi('M3 S60'); done()
    c.mdi('G33 Z1 K1')
    wait(lambda: hal.get_value('spindle.0.index-enable'), 'fresh index edge after reacquire')
    pin('spindle.0.index-enable', 0)
    settle()
    assert not hal.get_value('spindle.0.index-enable'), 'encoder index handshake failed'
    c.abort(); done()

elif case in ('jogstop', 'joghome'):
    release('joint.1')
    c.teleop_enable(0); done()
    c.set_joint_availability(1, True)
    wait(lambda: hal.get_value('joint.1.availability-request'), 'acquisition waiting for ACK')
    c.jog(linuxcnc.JOG_STOP, 1, 1)
    settle()
    assert hal.get_value('joint.1.availability-request'), 'jog stop canceled acquisition'
    pin('joint.1.availability-ack', 1)
    if case == 'joghome':
        wait(lambda: hal.get_value('joint.1.index-enable'), 'acquisition homing index')
        c.jog(linuxcnc.JOG_STOP, 1, 1)
        settle()
        assert hal.get_value('joint.1.index-enable'), 'jog stop canceled acquisition homing'
        assert hal.get_value('joint.1.availability-request')
        pin('joint.1.index-enable', 0)
    done()
    wait(lambda: s.joint[1]['available'], 'acquisition completed after jog stop')

elif case == 'fault':
    acquire('spindle.0')
    run('dwell.ngc')
    wait(lambda: s.exec_state == linuxcnc.EXEC_WAITING_FOR_DELAY and
         s.motion_mode == linuxcnc.TRAJ_MODE_COORD, 'coordinated dwell')
    assert s.spindle[0]['availability_state'] == 3
    Path('abort-reasons.log').unlink(missing_ok=True)
    pin('spindle.0.availability-ack', 0)
    wait(lambda: s.interp_state == linuxcnc.INTERP_IDLE, 'task abort')
    settle(1.3)
    assert not hal.get_value('motion.digital-out-00'), 'program continued after ACK loss'
    assert not s.enabled
    reasons = [float(line) for line in Path('abort-reasons.log').read_text().splitlines()]
    assert 3 in reasons, f'task did not handle motion RCS_ERROR: {reasons}'
    # The resource fault stays visible, but acknowledged motion errors must
    # not prevent a deliberate release, re-enable and new program.
    c.abort(); done()
    for resource in ('spindle.0',):
        pin(resource + '.availability-ack', 0)
    settle()
    c.mode(linuxcnc.MODE_MANUAL); done()
    for resource in ('spindle.0',):
        release(resource)
    c.state(linuxcnc.STATE_ON); done()
    acquire('spindle.0')
    run('dwell.ngc'); done()
    wait(lambda: bool(hal.get_value('motion.digital-out-00')), 'deliberate recovery')

elif case == 'wheel':
    c.teleop_enable(1); done()
    pin('axis.x.jog-enable', 1)
    pin('axis.x.jog-scale', .1)
    for cancel in (False, True):
        before = hal.get_value('joint.0.pos-cmd')
        c.set_spindle_availability(0, True)
        wait(lambda: hal.get_value('spindle.0.availability-request'), 'wheel ACK wait')
        counts = hal.get_value('axis.x.jog-counts')
        pin('axis.x.jog-counts', counts + 10)
        settle()
        assert hal.get_value('joint.0.pos-cmd') == before
        if cancel:
            c.abort(); done()
        else:
            pin('spindle.0.availability-ack', 1); done()
        settle(.3)
        assert abs(hal.get_value('joint.0.pos-cmd') - before) < 1e-9, 'delayed inhibited wheel jog'
        pin('axis.x.jog-counts', counts + 11)
        wait(lambda: abs(hal.get_value('joint.0.pos-cmd') - before - .1) < 1e-6,
             'fresh wheel count')
        if not cancel:
            release('spindle.0')

elif case == 'jointwheel':
    c.teleop_enable(0); done()
    release('joint.1')
    pin('joint.1.jog-enable', 1)
    pin('joint.1.jog-scale', .1)
    before = hal.get_value('joint.1.pos-cmd')
    counts = hal.get_value('joint.1.jog-counts')
    pin('joint.1.jog-counts', counts + 10)
    settle()
    acquire('joint.1')
    settle(.3)
    assert abs(hal.get_value('joint.1.pos-cmd') - before) < 1e-9, \
        'unavailable joint replayed stale wheel counts'
    pin('joint.1.jog-counts', counts + 11)
    wait(lambda: abs(hal.get_value('joint.1.pos-cmd') - before - .1) < 1e-6,
         'fresh joint wheel count')
    wait(lambda: s.inpos, 'FREE jog completion')
    release('joint.1')
    assert not s.joint[1]['availability_fault'], 'FREE-mode release faulted'

elif case == 'tail':
    c.teleop_enable(1); done()
    # Release may begin after logical motion finishes, but it must keep the
    # hardware request asserted until interpolation and compensation settle.
    for increment in (.01, .019):
        before = hal.get_value('joint.1.pos-cmd')
        for latch in (2, 3, 4): pin(f'flipflop.{latch}.reset', 1)
        settle()
        for latch in (2, 3, 4): pin(f'flipflop.{latch}.reset', 0)
        c.jog(linuxcnc.JOG_INCREMENT, 0, 1, 1, increment)
        wait(lambda: abs(s.joint_position[1] - before - increment) < 1e-6
             and hal.get_value('motion.in-position'), 'TELEOP endpoint')
        # Task admission observes NML completion after the servo endpoint.
        done()
        assert hal.get_value('flipflop.2.out'), 'TELEOP interpolation tail not exercised'
        assert hal.get_value('flipflop.3.out'), 'upstream INPOS did not precede interpolation completion'
        c.set_joint_availability(1, False)
        wait(lambda: not hal.get_value('joint.1.availability-request'), 'settled release')
        assert not hal.get_value('flipflop.4.out'), \
            'amplifier disabled before release tail settled'
        held = hal.get_value('joint.1.pos-cmd')
        settle(.02)
        assert abs(hal.get_value('joint.1.backlash-vel')) < 1e-12
        assert abs(hal.get_value('joint.1.backlash-corr') -
                   hal.get_value('joint.1.backlash-filt')) < 1e-12
        pin('joint.1.availability-ack', 0); done()
        x = s.joint_position[0]
        c.jog(linuxcnc.JOG_INCREMENT, 0, 0, 1, .01)
        wait(lambda: abs(s.joint_position[0] - x - .01) < 1e-6
             and hal.get_value('motion.in-position'), 'unrelated TELEOP move')
        settle()
        assert abs(hal.get_value('joint.1.pos-cmd') - held) < 1e-12, 'released TELEOP joint moved'
        acquire('joint.1')

elif case == 'amp':
    release('joint.1')
    pin('joint.1.amp-fault-in', 1)
    pin('flipflop.0.reset', 1); settle()
    pin('flipflop.0.reset', 0)
    c.set_joint_availability(1, True)
    wait(lambda: hal.get_value('joint.1.availability-request'), 'faulted acquisition')
    pin('joint.1.availability-ack', 1)
    wait(lambda: not s.enabled, 'fault inhibit')
    settle()
    assert not hal.get_value('flipflop.0.out'), 'amp-enable pulsed with amp-fault asserted'

elif case == 'slowtask':
    # fixture.py acquired joint 1, which homes through the queued home cycle
    assert s.homed[1], s.homed
    assert s.joint[1]['available']
    assert not errors, errors

else:
    raise AssertionError('unknown scenario ' + case)
print(f'availability regression {case} ok')
