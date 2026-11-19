#!/usr/bin/env python3
from common import *

start()
print("*** after the restart: T1 loaded, its stored values back, no offset")
check_state(1, (5.0, 10.0), (0.3, -0.1), (0.0, 0.0))
print("*** G43 applies T1's geometry plus wear")
mdi("g43")
check_state(1, (5.0, 10.0), (0.3, -0.1), (5.3, 9.9))
print("*** M6 to T2 and back applies each tool's geometry plus wear")
mdi("t2 m6")
l.wait_for_tool_in_spindle(2)
check_state(2, (-2.5, 20.0), (-0.3, 0.15), (-2.8, 20.15))
mdi("t1 m6")
l.wait_for_tool_in_spindle(1)
check_state(1, (5.0, 10.0), (0.3, -0.1), (5.3, 9.9))
print("*** all checks passed")
sys.exit(0)
