import os
import re
from pathlib import Path
import subprocess
import tempfile
root = Path(os.environ['EMC2_HOME'])
source = (root / 'src/emc/motion/control.c').read_text()
def function(name, source=source):
    start = re.search(r'^(?:static )?(?:int|void) ' + name + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
preamble = r"""
#include <assert.h>
#include <math.h>
#include <string.h>
#include "cubic.h"
#define _(s) s
#define reportError(...) ((void)0)
#define NO_OF_KINS_JOINTS 3
typedef struct { double x; } EmcPose;
typedef struct {
    double coarse_pos;
    CUBIC_STRUCT cubic;
} emcmot_joint_t;
static emcmot_joint_t joints[3];
typedef int KINEMATICS_FORWARD_FLAGS;
static int iflags, fflags;
static int ik_result;
static double solution[3];
static int kinematicsInverse(const EmcPose *p, double *out, int *i, int *f) {
    *f = *f + 1;
    memcpy(out, solution, sizeof(solution)); return ik_result;
}
"""
# The second patch supplies additional lifecycle/held-reference test fixtures.
extension = Path(__file__).with_name('availability-fixture.h')
if extension.exists(): preamble += extension.read_text()
main = r"""
int main(void) {
    double positions[3]; CUBIC_STRUCT staged[3]; EmcPose pose = {0};
    for (int n=0; n<3; n++) {
        cubicInit(&joints[n].cubic);
        cubicSetSegmentTime(&joints[n].cubic, .004);
        cubicSetInterpolationRate(&joints[n].cubic, 4);
        joints[n].coarse_pos = 20+n;
        solution[n] = n;
    }
    unsigned char before[sizeof(joints)]; memcpy(before, joints, sizeof(joints));
    fflags = 10;
    ik_result = -1;
    assert(prepare_joint_targets(&pose, positions, staged) != 0);
    assert(fflags == 10);
    ik_result = 0;
    solution[2] = NAN;
    assert(prepare_joint_targets(&pose, positions, staged) != 0);
    assert(memcmp(before, joints, sizeof(joints)) == 0);
    assert(fflags == 10);
    solution[2] = 2;
    assert(prepare_joint_targets(&pose, positions, staged) == 0);
    assert(memcmp(before, joints, sizeof(joints)) == 0);
    assert(fflags == 11);
    assert(positions[0] == 0 && positions[1] == 1 && positions[2] == 2);
    joints[2].cubic.configured = 0;
    memcpy(before, joints, sizeof(joints));
    assert(prepare_joint_targets(&pose, positions, staged) != 0);
    assert(memcmp(before, joints, sizeof(joints)) == 0);
    assert(fflags == 11);
    joints[2].cubic.configured = joints[0].cubic.configured;
#ifdef TEST_AVAILABILITY
    test_availability(&pose, positions, staged);
#endif
}
"""
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    # Only the math header is shimmed; cubic.c is compiled unchanged.
    (tmp/'rtapi_math.h').write_text('#include <math.h>\n')
    text = preamble + '\n' + function('prepare_joint_targets')
    extra = Path(__file__).with_name('availability-cases.h')
    if extra.exists(): text += '\n' + extra.read_text()
    (tmp/'test.c').write_text(text + '\n' + main)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-I'+str(tmp),
                    '-I'+str(root/'src/emc/kinematics'), '-I'+str(root/'src/emc/motion'), str(tmp/'test.c'),
                    str(root/'src/emc/kinematics/cubic.c'), '-lm', '-o', str(tmp/'test')], check=True)
    subprocess.run([str(tmp/'test')], check=True)

# Exercise the production legacy activation command handlers in every lifecycle
# state: controlled joints must retain ACTIVE and all lifecycle fields on error.
command_source = (root / 'src/emc/motion/command.c').read_text()
start = command_source.index('\tcase EMCMOT_JOINT_ACTIVATE:')
end = command_source.index('\tcase EMCMOT_JOINT_HOME:', start)
activation_cases = command_source[start:end]
activation_test = r"""
#include <assert.h>
#include <string.h>
#define _(s) s
#define reportError(...) ((void)0)
#define rtapi_print_msg(...) ((void)0)
#define SET_JOINT_ACTIVE_FLAG(j, v) ((j)->active = (v))
enum { EMCMOT_JOINT_ACTIVATE, EMCMOT_JOINT_DEACTIVATE,
       EMCMOT_COMMAND_INVALID_COMMAND };
typedef struct { int active; } joint_t;
static struct {
    int commandStatus;
    struct { struct { int controlled, state, phase; } availability; } joint_status[1];
} status, *emcmotStatus = &status;
static void command(int cmd, joint_t *joint) {
    int joint_num = 0;
    switch (cmd) {
""" + activation_cases + r"""
    }
}
int main(void) {
    for (int state = 0; state <= 5; ++state)
        for (int active = 0; active <= 1; ++active)
            for (int cmd = 0; cmd <= 1; ++cmd) {
                joint_t joint = {active};
                status.joint_status[0].availability.controlled = 1;
                status.joint_status[0].availability.state = state;
                status.joint_status[0].availability.phase = 42;
                status.commandStatus = 0;
                command(cmd, &joint);
                assert(joint.active == active);
                assert(status.commandStatus == EMCMOT_COMMAND_INVALID_COMMAND);
                assert(status.joint_status[0].availability.state == state);
                assert(status.joint_status[0].availability.phase == 42);
            }
    joint_t joint = {0};
    status.joint_status[0].availability.controlled = 0;
    status.commandStatus = 0;
    command(EMCMOT_JOINT_ACTIVATE, &joint);
    assert(joint.active == 1 && status.commandStatus == 0);
    command(EMCMOT_JOINT_DEACTIVATE, &joint);
    assert(joint.active == 0 && status.commandStatus == 0);
}
"""
with tempfile.TemporaryDirectory() as directory:
    source_file = Path(directory) / 'activation.c'
    binary = Path(directory) / 'activation'
    source_file.write_text(activation_test)
    subprocess.run(['cc', '-Wall', '-Werror', str(source_file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
