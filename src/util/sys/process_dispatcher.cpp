
#include "process_dispatcher.hpp"

#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <string>

#include "util/sys/proc.hpp"
#include "util/logger.hpp"
#include "util/sys/fileutils.hpp"

void ProcessDispatcher::dispatch() {

    const char* tmpdirCStr = std::getenv("MALLOB_TMP_DIR");
    const std::string tmpdir = tmpdirCStr ? tmpdirCStr : "/tmp";

    // Read command from tmp file
    const pid_t myPid = Proc::getPid();
    const std::string commandOutfile = tmpdir + "/mallob_subproc_cmd_" + std::to_string(myPid);
    while (!FileUtils::exists(commandOutfile)) {
        usleep(1000);
    }
    const auto f = fopen(commandOutfile.c_str(), "r");
    int size;
    fread(&size, sizeof(int), 1, f);
    char str[size];
    fread(str, 1, size, f);
    fclose(f);
    FileUtils::rm(commandOutfile); // clean up immediately

    std::string command(str, size);

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
