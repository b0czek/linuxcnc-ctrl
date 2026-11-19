#!/usr/bin/env python3
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'runtime-availability-regressions'))
from fixture import *

if case == 'sync':
    acquire('spindle.0')
    c.mode(linuxcnc.MODE_MDI); done()
    c.mdi('S1000 G95 F1'); done()
    c.mdi('M55 $0')
    wait(lambda: not hal.get_value('spindle.0.availability-request'), 'M55 release')
    pin('spindle.0.availability-ack', 0); done()
    c.mdi('G1 X1')
    result = c.wait_complete(2)
    assert result == linuxcnc.RCS_ERROR, 'feed inherited unavailable spindle sync'
    assert abs(hal.get_value('joint.0.pos-cmd')) < 1e-9
    c.abort(); done()
    c.mdi('G94'); done()
    c.mdi('G0 X1'); done()
    wait(lambda: abs(s.joint_position[0] - 1) < 1e-6,
         'G94 did not allow unrelated motion')

elif case == 'step':
    release('joint.1')
    run('step.ngc')
    wait(lambda: hal.get_value('joint.1.availability-request'), 'M54 ACK wait')
    c.auto(linuxcnc.AUTO_PAUSE)
    wait(lambda: s.interp_state == linuxcnc.INTERP_PAUSED, 'AUTO pause')
    def held_at(position, output=False):
        assert s.enabled and s.task_mode == linuxcnc.MODE_AUTO
        assert abs(s.joint_position[1] - position) < 1e-6
        assert bool(hal.get_value('motion.digital-out-00')) == output
        assert not hal.get_value('motion.digital-out-01'), 'later output escaped Single Step'

    # Pausing at the ACK barrier must block the move and both later outputs.
    settle(.2)
    held_at(0)
    assert s.interp_state == linuxcnc.INTERP_PAUSED
    assert not s.joint[1]['available']
    pin('joint.1.availability-ack', 1)
    wait(lambda: s.joint[1]['available'], 'acquisition completion')
    settle(.2)
    held_at(0)
    assert s.interp_state == linuxcnc.INTERP_PAUSED

    def step():
        c.auto(linuxcnc.AUTO_STEP)
        serial = c.serial
        wait(lambda: s.echo_serial_number >= serial, 'Single Step acknowledgement')
        settle(.2)

    # The first step clears the completion/synchronization boundary only.
    # The next canonical motion line is waiting; neither motion nor output
    # may escape without another explicit step.
    step()
    held_at(0)
    assert s.current_line == 2, 'Single Step did not reach the first motion boundary'
    settle(.3)
    held_at(0)

    # Immediate M64 output carries the preceding motion's canonical line id.
    # Step that group, then prove the next move/output group is still blocked.
    step()
    wait(lambda: abs(s.joint_position[1] - 1) < 1e-6
         and hal.get_value('motion.digital-out-00'), 'stepped motion/output group')
    held_at(1, True)
    assert s.current_line == 4, 'Single Step did not stop at the next motion boundary'
    settle(.3)
    held_at(1, True)

else:
    raise AssertionError(case)
print(f'availability M-code regression {case} ok')
