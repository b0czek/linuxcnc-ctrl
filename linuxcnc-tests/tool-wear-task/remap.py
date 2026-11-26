import emccanon
from interpreter import INTERP_OK


def out_of_range_tools(self, **words):
    """Report what emccanon.GET_EXTERNAL_TOOL_TABLE gives for indexes
    outside the table: an empty entry, wear included."""
    for idx in (-1, 100000):
        t = emccanon.GET_EXTERNAL_TOOL_TABLE(idx)
        w = t.wear_offset
        emccanon.MESSAGE("out-of-range %d: toolno %d wear %r"
                         % (idx, t.toolno,
                            [w.tran.x, w.tran.y, w.tran.z, w.a, w.b, w.c,
                             w.u, w.v, w.w]))
    return INTERP_OK
