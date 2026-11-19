#!/usr/bin/env python3
"""Check actual execute return codes using the production interpreter/canon."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(os.environ['EMC2_HOME'])
with tempfile.TemporaryDirectory() as directory:
    binary = str(Path(directory) / 'execute-status')
    objects = root / 'src/objects/emc'
    command = [os.environ.get('CXX', 'c++'), 'execute-status.cc',
               '-I' + str(root / 'include'),
               '-I' + str(root / 'src'),
               '-I' + str(root / 'src/emc'),
               '-I' + str(root / 'src/emc/nml_intf'),
               '-I' + str(root / 'src/emc/rs274ngc'),
               str(objects / 'sai/saicanon.o'),
               str(objects / 'sai/dummyemcstat.o'),
               str(objects / 'task/taskclass.o'),
               '-L' + str(root / 'lib'), '-Wl,-rpath,' + str(root / 'lib'),
               '-lrs274', '-llinuxcnc', '-lnml', '-llinuxcnchal',
               '-llinuxcncini', '-lpyplugin', '-ltooldata']
    command += subprocess.check_output(['python3-config', '--embed', '--ldflags'], text=True).split()
    subprocess.run(command + ['-o', binary], check=True)
    for ini, program, finishes in [('remap.ini', 'remapped.ngc', '0'),
                                   ('test.ini', 'valid-joint.ngc', '2'),
                                   ('test.ini', 'valid-spindle.ngc', '2')]:
        result = subprocess.run([binary, ini, program, finishes], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f'{ini} {program}: {result.returncode}\n{result.stdout}\n{result.stderr}')
