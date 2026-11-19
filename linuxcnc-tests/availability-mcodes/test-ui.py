#!/usr/bin/env python3
import os
import time
import linuxcnc
import hal
h = hal.component('availability-mcode-test'); h.ready()
c = linuxcnc.command(); s = linuxcnc.stat()
def pin(n,v): hal.set_p(n,str(v))
def wait(fn, message):
    end = time.monotonic()+10
    while time.monotonic()<end:
        s.poll()
        if fn(): return
        time.sleep(.005)
    raise AssertionError(message)
def done(): assert c.wait_complete(8) == linuxcnc.RCS_DONE
def run(file):
    c.program_open(os.path.abspath(file)); done()
    c.auto(linuxcnc.AUTO_RUN, 0)
def idle(): wait(lambda: s.interp_state == linuxcnc.INTERP_IDLE and s.exec_state == linuxcnc.EXEC_DONE, 'program completion')
wait(lambda: s.task_state == linuxcnc.STATE_ESTOP, 'startup')
c.state(linuxcnc.STATE_ESTOP_RESET); done()
c.state(linuxcnc.STATE_ON); done()
c.home(0); done()
wait(lambda: s.homed[0], 'home X')
c.mode(linuxcnc.MODE_MDI); done()
for code in ('M54 P99', 'M54 $99'):
    c.mdi(code)
    assert c.wait_complete(3) == linuxcnc.RCS_ERROR, code + ' was not rejected by task'
    c.abort(); done()
c.mode(linuxcnc.MODE_AUTO); done()
if os.environ.get('AVAILABILITY_NO_FORCE'):
    # Forced-homing policy does not bypass acquisition homing. The unhomed
    # controlled joint is referenced individually; Home All skips it.
    run('unhomed.ngc')
    wait(lambda: hal.get_value('joint.1.availability-request'), 'unhomed request')
    pin('joint.1.availability-ack',1)
    done(); idle()
    assert abs(s.joint_position[1]-10)<.001
    # Once homed, direct acquisition validates and preserves the held position.
    c.mode(linuxcnc.MODE_MDI); done()
    c.set_joint_availability(1, False)
    wait(lambda: not hal.get_value('joint.1.availability-request'), 'direct release')
    pin('joint.1.availability-ack', 0); done()
    c.set_joint_availability(1, True)
    wait(lambda: hal.get_value('joint.1.availability-request'), 'direct acquire')
    assert c.wait_complete(.05) == -1, 'completed before ACK'
    pin('joint.1.availability-ack', 1); done()
    c.mdi('G91 G0 Y1'); done(); s.poll()
    assert abs(s.joint_position[1]-11)<.001, 'direct acquisition changed held position'
    print('availability without forced homing ok')
    raise SystemExit(0)
c.mode(linuxcnc.MODE_MANUAL); done()
c.teleop_enable(0); done()
# Controlled joints are acquired individually; Home All skips them.
c.set_joint_availability(1,True)
wait(lambda: hal.get_value('joint.1.availability-request'), 'manual request')
pin('joint.1.availability-ack',1); done()
c.set_joint_availability(1,False)
wait(lambda: not hal.get_value('joint.1.availability-request'), 'manual release')
pin('joint.1.availability-ack',0); done()
c.mode(linuxcnc.MODE_AUTO); done()
run('ordered.ngc')
wait(lambda: hal.get_value('joint.1.availability-request'), 'M54 request')
assert abs(s.joint_position[0]-1)<.001, 'prior move not finished'
time.sleep(.05); s.poll()
assert abs(s.joint_position[0]-1)<.001, 'continued before ACK'
c.set_joint_availability(1, False)
assert c.wait_complete(.2) == linuxcnc.RCS_ERROR, 'direct release interrupted AUTO M54'
assert hal.get_value('joint.1.availability-request')
c.program_stop()
time.sleep(.05); s.poll()
assert hal.get_value('joint.1.availability-request'), 'Stop canceled acquisition'
assert s.interp_state != linuxcnc.INTERP_IDLE, 'Stop aborted program'
pin('joint.1.availability-ack',1)
wait(lambda: s.joint[1]['available'], 'M54 completion')
wait(lambda: s.stop_state == linuxcnc.STOP_STOPPED, 'Stop at acquisition boundary')
assert abs(s.joint_position[0]-1)<.001
c.auto(linuxcnc.AUTO_RESUME)
wait(lambda: not hal.get_value('joint.1.availability-request'), 'M55 request')
assert abs(s.joint_position[0]-2)<.001
assert abs(s.joint_position[1]-1)<.001, 'held position changed across M54/M55'
held = hal.get_value('joint.1.pos-cmd')
time.sleep(.05); s.poll()
assert hal.get_value('joint.1.pos-cmd') == held, 'command moved after release'
assert abs(s.joint_position[0]-2)<.001, 'continued before release ACK'
pin('joint.1.availability-ack',0)
done(); idle()
assert abs(s.joint_position[0]-3)<.001 and abs(s.joint_position[1]-1)<.001
# The same programmed barrier works from MDI, without idle-only admission.
c.mode(linuxcnc.MODE_MDI); done()
c.mdi('M54 P1')
wait(lambda: hal.get_value('joint.1.availability-request'), 'MDI M54')
pin('joint.1.availability-ack', 1); done()
c.mdi('G91 G0 Y1'); done(); s.poll()
assert abs(s.joint_position[1]-2)<.001
c.mdi('M55 P1')
wait(lambda: not hal.get_value('joint.1.availability-request'), 'MDI M55')
pin('joint.1.availability-ack', 0); done()
c.mode(linuxcnc.MODE_AUTO); done()
# Abort must terminate AUTO at the barrier, both before ACK and during homing;
# Machine Off cancels acquisition homing the same way.
for during_home, cancel in ((False, c.abort), (True, c.abort),
                            (True, lambda: c.state(linuxcnc.STATE_OFF))):
    run('homing.ngc')
    wait(lambda: hal.get_value('joint.2.availability-request'), 'abort AUTO request')
    if during_home:
        pin('joint.2.availability-ack', 1)
        wait(lambda: abs(hal.get_value('joint.2.vel-cmd')) > .1, 'AUTO homing motion')
    cancel()
    wait(lambda: not hal.get_value('joint.2.availability-request'), 'AUTO abort output')
    pin('joint.2.availability-ack', 0)
    done(); idle()
    assert abs(s.joint_position[0]-4)<.001, 'Abort continued after M54'
    pin('joint.2.availability-ack', 1)
    time.sleep(.05); s.poll()
    assert not s.joint[2]['available']
    assert not hal.get_value('joint.2.amp-enable-out')
    assert abs(s.joint_position[0]-4)<.001, 'late ACK resumed aborted program'
    pin('joint.2.availability-ack', 0)
    c.state(linuxcnc.STATE_OFF); c.wait_complete(3)
    wait(lambda: s.exec_state == linuxcnc.EXEC_DONE, 'fault cleanup')
    for n in (1, 2):
        c.set_joint_availability(n, False); done()
    c.state(linuxcnc.STATE_ON); done()
    c.mode(linuxcnc.MODE_MANUAL); done()
    c.home(0); done()
    wait(lambda: s.homed[0], 'rehome X after abort cleanup')
    c.mode(linuxcnc.MODE_AUTO); done()
# The next fresh program can acquire and home successfully.
run('homing.ngc')
wait(lambda: hal.get_value('joint.2.availability-request'), 'M54 home request')
pin('joint.2.availability-ack',1)
for state, switch in [(7,1),(10,0),(12,1)]:
    wait(lambda: hal.get_value('joint.2.home-state') == state, 'home phase')
    assert abs(s.joint_position[0]-4)<.001, 'continued before homing finished'
    assert not hal.get_value('joint.2.available')
    pin('joint.2.home-sw-in',switch)
done(); idle()
assert s.joint[2]['available'] and abs(s.joint_position[0]-5)<.001
# Stop during M54 acquisition homing behaves like Stop during G28.2: it is
# accepted, stays STOPPING until homing ends, and holds the next block.
c.mode(linuxcnc.MODE_MDI); done()
c.mdi('M55 P2')
wait(lambda: not hal.get_value('joint.2.availability-request'), 'release Z')
pin('joint.2.availability-ack', 0); done()
assert not s.joint[2]['homed']
pin('joint.2.home-sw-in', 0)
c.mode(linuxcnc.MODE_AUTO); done()
run('homing.ngc')
wait(lambda: hal.get_value('joint.2.availability-request'), 'Stop M54 request')
pin('joint.2.availability-ack', 1)
wait(lambda: s.exec_state == getattr(linuxcnc, 'EXEC_WAITING_FOR_HOMING', 12) and
     s.joint[2]['homing'], 'M54 queued homing cycle')
c.program_stop()
wait(lambda: s.stop_state == linuxcnc.STOP_STOPPING, 'Stop during M54 homing')
for state, switch in [(7,1),(10,0),(12,1)]:
    wait(lambda: hal.get_value('joint.2.home-state') == state, 'stopping home phase')
    assert s.stop_state == linuxcnc.STOP_STOPPING, 'Stop ended before homing'
    pin('joint.2.home-sw-in',switch)
wait(lambda: s.stop_state == linuxcnc.STOP_STOPPED, 'Stop after M54 homing')
assert s.joint[2]['available'] and s.task_mode == linuxcnc.MODE_AUTO
time.sleep(.1); s.poll()
assert abs(s.joint_position[0]-4)<.001, 'continued after M54 while stopped'
c.auto(linuxcnc.AUTO_RESUME)
idle()
assert abs(s.joint_position[0]-5)<.001
# Stop or Pause before the ACK of an unhomed joint: the ACK completes the
# handshake, but like a queued G28.2 the acquisition home starts only after
# Resume. Time held exceeds AVAILABILITY_TIMEOUT, which does not run then.
for hold in (c.program_stop, lambda: c.auto(linuxcnc.AUTO_PAUSE)):
    c.mode(linuxcnc.MODE_MDI); done()
    c.mdi('M55 P2')
    wait(lambda: not hal.get_value('joint.2.availability-request'), 'release Z before hold')
    pin('joint.2.availability-ack', 0); done()
    pin('joint.2.home-sw-in', 0)
    c.mode(linuxcnc.MODE_AUTO); done()
    run('homing.ngc')
    wait(lambda: hal.get_value('joint.2.availability-request'), 'held M54 request')
    hold()
    if hold == c.program_stop:
        wait(lambda: s.stop_state == linuxcnc.STOP_STOPPED, 'Stop before ACK')
    else:
        wait(lambda: s.interp_state == linuxcnc.INTERP_PAUSED, 'Pause before ACK')
    pin('joint.2.availability-ack', 1)
    time.sleep(2.5); s.poll()
    assert not s.joint[2]['homing'] and hal.get_value('joint.2.home-state') == 0, \
        'acquisition homing started while held'
    assert abs(hal.get_value('joint.2.vel-cmd')) < 1e-9, 'Z moved while held'
    assert s.joint[2]['availability_fault'] == 0, 'acquisition timed out while held'
    assert hal.get_value('joint.2.availability-request')
    assert not s.joint[2]['available']
    assert abs(s.joint_position[0]-4)<.001, 'continued after M54 while held'
    c.auto(linuxcnc.AUTO_RESUME)
    for state, switch in [(7,1),(10,0),(12,1)]:
        wait(lambda: hal.get_value('joint.2.home-state') == state, 'home phase after Resume')
        assert abs(s.joint_position[0]-4)<.001, 'continued before homing finished'
        pin('joint.2.home-sw-in',switch)
    idle()
    assert s.joint[2]['available'] and abs(s.joint_position[0]-5)<.001
    assert s.stop_state == linuxcnc.STOP_IDLE and not s.task_paused
run('timeout.ngc')
wait(lambda: s.spindle[0]['availability_fault'] != 0, 'timeout propagation')
assert abs(s.joint_position[0]-6)<.001, 'continued after failed acquisition'
print('availability M-code scenarios ok')
