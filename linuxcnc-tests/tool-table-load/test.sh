#!/bin/bash
set -e

# run 1: a table with CRLF line ends, blank lines and a comment-only line
# loads whole
printf 'T1 P1 Z+1.000000 ;first\r\n\r\n   \r\n ;only a comment\r\nT2 P2 Z+2.000000 \r\nT3 P3 Z+3.000000 ;third\r\n' > tool.tbl
rm -f sim.var
../tool-wear-common/make-ini.sh run.ini ui=./crlf-ui.py
linuxcnc -r run.ini

# run 2: a table with a line that is not a tool line does not load, and is
# not saved over until a table loads
printf 'T1 P1 Z+1.000000\nnot a tool line\nT2 P2 Z+2.000000\n' > tool.tbl
rm -f sim.var
../tool-wear-common/make-ini.sh run.ini ui=./garbage-ui.py
linuxcnc -r run.ini

# run 3: a nonrandom pocket number is only a label, so a table that loads at
# startup also reloads, and saving keeps working
printf 'T1 P1 Z+1.000000\nT99999 P99999 Z+2.000000\n' > tool.tbl
rm -f sim.var
../tool-wear-common/make-ini.sh run.ini ui=./reload-ui.py
linuxcnc -r run.ini

# run 4: a reload refused for a file that matches the table in use reports
# the refusal and does not stop saves
printf 'T1 P1 Z+1.000000\nT5 P5 Z+5.000000\nT5 P6 Z+6.000000\n' > tool.tbl
rm -f sim.var
linuxcnc -r run.ini

# the remaining runs choose their user interface and changer type
run_ui() {
    rm -f sim.var
    ../tool-wear-common/make-ini.sh run.ini "ui=./$1" "random=$2"
    linuxcnc -r run.ini
}

# run 5: a random changer whose table did not load at startup: a tool change
# does not save over the file, and the reload takes the spindle tool from it
printf 'T1 P1 Z+1.000000\nnot a tool line\n' > tool.tbl
run_ui random-start-ui.py 1

# run 6: a table file made while the machine runs is not saved over
rm -f tool.tbl
run_ui missing-ui.py 1

# run 7: nor is one changed or replaced since it was loaded
printf 'T1 P1 Z+1.000000\nT2 P2 Z+2.000000\n' > tool.tbl
run_ui external-edit-ui.py 0

# run 8: loading another file does not lift the protection of this one
printf 'T1 P1 Z+1.000000\nnot a tool line\n' > tool.tbl
printf 'T8 P8 Z+8.000000\nT9 P9 Z+9.000000\n' > other.tbl
run_ui other-table-ui.py 0

# run 9: saving through a symbolic link keeps the link
rm -rf tool.tbl linked
mkdir linked
printf 'T1 P1 Z+1.000000\nT2 P2 Z+2.000000\n' > linked/real.tbl
chmod 640 linked/real.tbl
ln -s real.tbl linked/alias.tbl
ln -s linked/alias.tbl tool.tbl
run_ui symlink-ui.py 0
rm -rf tool.tbl linked
