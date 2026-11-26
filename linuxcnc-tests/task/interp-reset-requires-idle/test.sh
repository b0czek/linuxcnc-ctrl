#!/bin/bash
# Tool edits save the table, so start from a fresh copy.
printf 'T1 P1 Z0 ;\n' > tool.tbl
exec linuxcnc -r test.ini
