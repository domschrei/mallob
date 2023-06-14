#include "bloqqer_caller.hpp"

#include <cstdlib>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

BloqqerCaller::FIFO::FIFO(std::string path, mode_t mode) {
  fd = mkfifo(path.c_str(), mode);
}
BloqqerCaller::FIFO::~FIFO() {
  close(fd);
}

BloqqerCaller::BloqqerCaller() {

}
BloqqerCaller::~BloqqerCaller() {

}

int BloqqerCaller::process(PCNF &inOut, int jobId, int litToTry, int maxCost) {
  pid_t pid = getpid();
  std::string fifoPath = std::to_string(pid) + "." + std::to_string(jobId);
  std::string command = "qbf_bloqqer "
    + fifoPath
    + " --maxexpvarcost=" + std::to_string(maxCost)
    + " --expvar=" + std::to_string(litToTry);

  FIFO fifo{fifoPath, 0600};
  FILE* bloqqer = popen(command.c_str(), "r");

  int res = pclose(bloqqer);

  return WEXITSTATUS(res);
}

