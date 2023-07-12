
#pragma once

#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/process.hpp"
#include "util/assert.hpp"
#include "util/sys/tmpdir.hpp"
#include <ctime>
#include <string>
#include <fstream>
#include <unistd.h>

/*
Interface for starting subprocesses. Since some MPI stacks *hate* subprocessing
where an MPI process is the parent, this is done in a quite awkward manner to be
absolutely safe: The forked process does not touch any non-constant memory and
immediately executes a generic, parameter-free "dispatcher" executable.
The parent process then communicates the actual command and args to execute to
the dispatcher via a tmp file qualified by the dispatcher's PID.
Obviously this results in additional overhead, so spawning a subprocess via this
interface should be done sparingly.
*/
class Subprocess {

private:
    const Parameters& _params;
    const std::string _cmd;

public:
    Subprocess(const Parameters& params, const std::string& cmd) : _params(params), _cmd(cmd) {
        assert(!_cmd.empty());
    }

    pid_t start() {
        // FORK: Create a child process
        pid_t res = Process::createChild();
        if (res == 0) {
            // [child process]
            // Danger zone: Do not touch any memory.
            execl(MALLOB_SUBPROC_DISPATCH_PATH"mallob_process_dispatcher", 
                MALLOB_SUBPROC_DISPATCH_PATH"mallob_process_dispatcher", 
                (char*) 0);
            
            // If this is reached, something went very wrong with execvp
            LOG(V0_CRIT, "[ERROR] execl returned errno %i\n", (int)errno);
            abort();
        }

        // [parent process]
        // Assemble SAT subprocess command
        std::string executable;
        if (_cmd[0] == '/') executable = _cmd;
        else executable = std::string(MALLOB_SUBPROC_DISPATCH_PATH) + "/" + _cmd;
        //char* const* argv = _params.asCArgs(executable.c_str());
        std::string command = _params.getSubprocCommandAsString(executable.c_str());
        
        // Write command to tmp file (to be read by child process)
        std::string commandOutfile = TmpDir::get() + "/mallob_subproc_cmd_" + std::to_string(res) + "~";
        {
            std::ofstream ofs(commandOutfile);
            ofs << command << " " << std::endl;
        }
        std::rename(commandOutfile.c_str(), commandOutfile.substr(0, commandOutfile.size()-1).c_str()); // remove tilde
        return res;
    }

};
