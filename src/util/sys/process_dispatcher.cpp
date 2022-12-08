
#include "process_dispatcher.hpp"

#include <sys/types.h>
#include <stdlib.h>
#include <fstream>
#include <cstdio>

#include "util/assert.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/process.hpp"
#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/fileutils.hpp"

void ProcessDispatcher::dispatch() {

    // Read command from tmp file
    pid_t myPid = Proc::getPid();
    std::string commandOutfile = "/tmp/mallob_subproc_cmd_" + std::to_string(myPid);
    std::ifstream ifs(commandOutfile);
    while (!ifs.is_open()) {
        usleep(100);
        ifs = std::ifstream(commandOutfile);
    }
    std::string command((std::istreambuf_iterator<char>(ifs)),
                    (std::istreambuf_iterator<char>()));

    // Assemble arguments list
    int numArgs = 0;
    for (size_t i = 0; i < command.size(); ++i) {
        if (command[i] == '\n') break;
        if (command[i] == ' ') numArgs++;
    }
    char* argv[numArgs+1];
    size_t argvIdx = 0;
    size_t argBegin = 0;
    for (size_t i = 0; i < command.size(); ++i) {
        if (command[i] == '\n') break;
        if (command[i] == ' ') {
            command[i] = '\0';
            argv[argvIdx++] = command.data()+argBegin;
            argBegin = i+1;
        }
    }
    argv[argvIdx++] = nullptr;
    assert(argvIdx == numArgs+1);

    // Execute the SAT process.
    int result = execv(argv[0], argv);
    
    // If this is reached, something went wrong with execvp
    LOG(V0_CRIT, "[ERROR] execv returned %i with errno %i\n", result, (int)errno);
    usleep(1000 * 500); // sleep 0.5s
}
