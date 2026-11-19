#include "rs274ngc.hh"
#include "interp_return.hh"
#include <cassert>
#include <cstdlib>
int _task = 0;
// taskclass.o reports tool-table errors to the operator; milltask and rs274
// provide this, a standalone interpreter driver has to.
int emcOperatorError(const char *, ...) { return 0; }
InterpBase *pinterp;
int main(int argc, char **argv) {
    assert(argc == 4);
    setenv("INI_FILE_NAME", argv[1], 1);
    auto interp = pinterp = makeInterp();
    assert(interp->init() == INTERP_OK);
    assert(interp->open(argv[2]) == INTERP_OK);
    int finishes = 0;
    for (int n = 0; n < 100; n++) {
        int read = interp->read();
        assert(read == INTERP_OK);
        int result = interp->execute();
        if (result == INTERP_EXIT) {
            assert(finishes == atoi(argv[3]));
            return 0;
        }
        assert(result == INTERP_OK || result == INTERP_EXECUTE_FINISH);
        if (result == INTERP_EXECUTE_FINISH) finishes++;
    }
    abort();
}
