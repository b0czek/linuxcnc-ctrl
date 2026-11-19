#!/usr/bin/env python3
from common import *

start()
mdi("t1 m6")
l.wait_for_tool_in_spindle(1)
mdi("g10 l3 p1 x0.3")
check_state(1, (5.0, 10.0), (0.3, -0.1), (5.2, 9.9))
print("*** run 1 done, T1 in the spindle")
sys.exit(0)
