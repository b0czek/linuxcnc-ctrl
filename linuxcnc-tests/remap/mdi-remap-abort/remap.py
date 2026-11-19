import emccanon
from interpreter import INTERP_EXECUTE_FINISH


def m401(self, **words):
    # Keep the remap mid-execution: dwell, wait for it to finish, repeat.
    while True:
        emccanon.DWELL(0.05)
        yield INTERP_EXECUTE_FINISH
