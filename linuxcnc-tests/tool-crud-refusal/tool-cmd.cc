// Send one tool command over NML and report how it ended:
//   tool-cmd NMLFILE delete TOOL
//   tool-cmd NMLFILE update TOOL [POCKET]
// Exit status: 0 RCS_DONE, 1 RCS_ERROR, 2 no result.
#include "emc.hh"
#include "emc_nml.hh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int finish(RCS_STAT_CHANNEL &stat, int serial)
{
    for (int i = 0; i < 10000; i++) {
        if (stat.peek() == EMC_STAT_TYPE) {
            auto *status = static_cast<EMC_STAT *>(stat.get_address());
            if (status->echo_serial_number - serial >= 0) {
                if (status->status == RCS_STATUS::DONE) return 0;
                if (status->status == RCS_STATUS::ERROR) return 1;
            }
        }
        usleep(1000);
    }
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    RCS_CMD_CHANNEL cmd(emcFormat, "emcCommand", "xemc", argv[1]);
    RCS_STAT_CHANNEL stat(emcFormat, "emcStatus", "xemc", argv[1]);
    if (!cmd.valid() || !stat.valid()) return 2;
    int serial;
    if (!strcmp(argv[2], "delete")) {
        EMC_TOOL_DELETE msg;
        msg.toolno = atoi(argv[3]);
        if (cmd.write(&msg)) return 2;
        serial = msg.serial_number;
    } else {
        EMC_TOOL_UPDATE msg;
        msg.toolno = atoi(argv[3]);
        if (argc > 4) {
            msg.pocket = atoi(argv[4]);
            msg.fields |= EMC_TOOL_UPDATE_POCKET;
        }
        if (cmd.write(&msg)) return 2;
        serial = msg.serial_number;
    }
    return finish(stat, serial);
}
