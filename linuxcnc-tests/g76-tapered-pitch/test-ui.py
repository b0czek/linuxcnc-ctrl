#!/usr/bin/env python3
"""Check the tapered G76 pitch check against the configured axis limits.

See README for how each spindle speed limit below is derived.
"""

import sys
import time

import linuxcnc


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


c = linuxcnc.command()
s = linuxcnc.stat()
e = linuxcnc.error_channel()


def wait_for(predicate, message, timeout=10.0):
    start = time.time()
    while time.time() - start < timeout:
        s.poll()
        if predicate():
            return
        time.sleep(0.01)
    fail(message)


def drain_errors():
    messages = []
    while True:
        error = e.poll()
        if not error:
            return messages
        messages.append(error[1])


def mdi(command):
    if c.wait_complete(10.0) == -1:
        fail("previous command did not complete before %r" % command)
    c.mdi(command)
    if c.wait_complete(30.0) != linuxcnc.RCS_DONE:
        fail("%r failed: %s" % (command, drain_errors()))


wait_for(lambda: s.task_state == linuxcnc.STATE_ESTOP, "LinuxCNC did not initialize")
c.state(linuxcnc.STATE_ESTOP_RESET)
c.state(linuxcnc.STATE_ON)
c.wait_complete()
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()

# The X axis allows 30 mm/min and the Z axis 600 mm/min.  In G8 the thread
# runs from X10 Z0 to X8 Z-20: the drive line moves 0.1 mm of X per mm of Z, so
# at pitch 1.5 X needs 0.15 mm/min per RPM and the limit is 200 RPM.
EXTERNAL = "g76 p1.5 x8 z-20 i-1 j.5 k1 h1 q29.5"
INTERNAL = "g76 p1.5 x7 z-20 i1 j.5 k1 h1 q29.5"
CYLINDRICAL = "g76 p1.5 z-20 i-1 j.5 k1 h1 q29.5"
DIAMETER = "g76 p1.5 x16 z-20 i-2 j1 k2 h1 q29.5"

TAPERED_X = "on the X axis, which exceeds its maximum velocity of 30"

cases = [
    # (description, setup, G76 block, None if accepted else expected text)
    ("external tapered thread just under the limit",
     ["g8 g97 s199 m3", "g0 x10 z0"], EXTERNAL, None),
    ("external tapered thread just over the limit",
     ["g8 g97 s201 m3", "g0 x10 z0"], EXTERNAL,
     ["G76 tapered thread pitch 1.50748 at spindle speed 201 needs 30.15 per "
      "minute", TAPERED_X]),
    ("cylindrical thread at the same speed does not move X while synchronized",
     ["g8 g97 s201 m3", "g0 x10 z0"], CYLINDRICAL, None),
    ("internal tapered thread just under the limit",
     ["g8 g97 s199 m3", "g0 x5 z0"], INTERNAL, None),
    ("internal tapered thread just over the limit",
     ["g8 g97 s201 m3", "g0 x5 z0"], INTERNAL,
     ["G76 tapered thread pitch 1.50748 at spindle speed 201", TAPERED_X]),
    ("diameter mode tapered thread just under the limit",
     ["g7 g97 s199 m3", "g0 x20 z0"], DIAMETER, None),
    ("diameter mode tapered thread just over the limit",
     ["g7 g97 s201 m3", "g0 x20 z0"], DIAMETER,
     ["G76 tapered thread pitch 1.50748 at spindle speed 201", TAPERED_X]),
    # G96 without D: the smallest radius of any synchronized move is 6, at the
    # end of the full-depth pass, so 200 RPM is reached at S7.53982.
    ("G96 tapered thread just under the limit",
     ["g8 g0 x10 z0", "g96 s7.5 m3"], EXTERNAL, None),
    ("G96 tapered thread just over the limit",
     ["g8 g0 x10 z0", "g96 s7.6 m3"], EXTERNAL,
     ["G76 tapered thread pitch 1.50748 reaches spindle speed 201.596 in "
      "constant surface speed mode", TAPERED_X]),
    # G96 with D: radius 6 at S100 would be 2653 RPM, so D is the bound.
    ("G96 tapered thread with D just under the limit",
     ["g8 g0 x10 z0", "g96 s100 d199 m3"], EXTERNAL, None),
    ("G96 tapered thread with D just over the limit",
     ["g8 g0 x10 z0", "g96 s100 d201 m3"], EXTERNAL,
     ["G76 tapered thread pitch 1.50748 reaches spindle speed 201 in "
      "constant surface speed mode", TAPERED_X]),
    ("G96 D limit stays modal for a later G76.1",
     ["g8 g0 x10 z0", "g96 s100 d201 m3"], EXTERNAL.replace("g76", "g76.1"),
     ["reaches spindle speed 201", TAPERED_X]),
    # A tapered entry first feeds in along X alone at the entry lead of
    # 1.75121, so X limits it to 17.131 RPM.
    ("tapered entry just under the limit",
     ["g8 g97 s17 m3", "g0 x10 z0"], EXTERNAL + " e2 l1", None),
    ("tapered entry just over the limit",
     ["g8 g97 s18 m3", "g0 x10 z0"], EXTERNAL + " e2 l1",
     ["G76 entry taper pitch 1.75121 at spindle speed 18 needs 31.5218 per "
      "minute", TAPERED_X]),
    # The exit leaves the decreasing drive line outwards, so its X component is
    # the thread height less the drive line's: 49.6902 RPM.
    ("tapered exit just under the limit",
     ["g8 g97 s49 m3", "g0 x10 z0"], EXTERNAL + " e2 l2", None),
    ("tapered exit just over the limit",
     ["g8 g97 s50 m3", "g0 x10 z0"], EXTERNAL + " e2 l2",
     ["G76 exit taper pitch 1.61694 at spindle speed 50 needs 30.187 per "
      "minute", TAPERED_X]),
]

for description, setup, block, expected in cases:
    print("case: %s: %s" % (description, block))
    for command in setup:
        mdi(command)
    s.poll()
    if s.queue != 0:
        fail("motion queue not empty before %r" % block)
    stale = drain_errors()
    if stale:
        fail("unexpected errors before %r: %s" % (block, stale))

    c.mdi(block)
    messages = []
    accepted = False
    deadline = time.time() + 10.0
    while time.time() < deadline:
        messages = drain_errors()
        if messages:
            break
        s.poll()
        if s.queue > 0:
            accepted = True
            break
        time.sleep(0.01)

    if expected is None:
        if not accepted:
            fail("%s: rejected or not started: %s" % (description, messages))
        # The check passed and the passes are queued; nothing more to see.
        c.abort()
        c.wait_complete()
        wait_for(lambda: s.queue == 0 and s.current_vel == 0.0,
                 "%s: did not stop after abort" % description)
        c.mode(linuxcnc.MODE_MDI)
        c.wait_complete()
        late = drain_errors()
        if late:
            fail("%s: errors after accepting: %s" % (description, late))
        print("  accepted")
    else:
        if accepted or not messages:
            fail("%s: not rejected" % description)
        text = " ".join(messages)
        for part in expected:
            if part not in text:
                fail("%s: expected %r in %r" % (description, part, text))
        c.wait_complete()
        print("  rejected: %s" % text)

print("g76 tapered pitch checks complete")
sys.exit(0)
