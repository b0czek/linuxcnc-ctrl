#!/usr/bin/env python3
# T1 is ordinary; T3's line is 600 characters, longer than a tool line may
# be, and is refused when the table is loaded. With "random", the table is
# for a random toolchanger: T2 is in the spindle (pocket 0).
import sys
head = "T3 P3 X+1 Z+2 D0."
tail = " WZ-0.25 ;too long"
t3 = head + "0" * (600 - len(head) - len(tail)) + tail
assert len(t3) == 600
if sys.argv[1:] == ["random"]:
    print("T2 P0 Z+1.000000 ;in the spindle")
print("T1 P1 X+5.000000 Z+10.000000 WX+0.200000 WZ-0.100000 Q3 ;front tool")
print(t3)
