
#include "process_dispatcher.hpp"

#include <assert.h>
#include <errno.h>
#include <fstream>
#include <iterator>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <string>

#include "util/sys/proc.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/tmpdir.hpp"

void ProcessDispatcher::dispatch() {

    // Read command from tmp file
    const pid_t myPid = Proc::getPid();
    const std::string commandOutfile = TmpDir::getMachineLocalTmpDir() + "/edu.kit.iti.mallob.subproc_cmd_" + std::to_string(myPid);
    while (!FileUtils::exists(commandOutfile)) {
        usleep(1000);
    }
    std::ifstream ifs(commandOutfile);
    std::string command((std::istreambuf_iterator<char>(ifs)),
                       (std::istreambuf_iterator<char>()));
    ifs.close();
    FileUtils::rm(commandOutfile); // clean up immediately

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
    printf("[ERROR] execv returned %i with errno %i\n", result, (int)errno);
    usleep(1000 * 500); // sleep 0.5s
}
