#include "bloqqer_caller.hpp"

#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

BloqqerCaller::FIFO::FIFO(std::string path, mode_t mode, const char* mode_) {
  int res = mkfifo(path.c_str(), mode);
  if(res != 0) throw std::runtime_error("Could not open fifo " + path);
  fifo = fopen(path.c_str(), mode_);
}
BloqqerCaller::FIFO::~FIFO() {
  fclose(fifo);
}

BloqqerCaller::BloqqerCaller(Logger &logger) : _log(logger) {

}
BloqqerCaller::~BloqqerCaller() {

}

#include	<stdio.h>
#include	<signal.h>

#define	READ	0
#define	WRITE	1

std::pair<FILE*, pid_t> popen2(const char* command, const char *mode) {
  int	pfp[2], pid;
  FILE	*fp;
  int	parent_end, child_end;

  if ( *mode == 'r' ){
    parent_end = READ;
    child_end = WRITE ;
  } else if ( *mode == 'w' ){
    parent_end = WRITE;
    child_end = READ ;
  } else return {NULL, 0} ;

  if ( pipe(pfp) == -1 )
    return {NULL, 0};
  if ( (pid = fork()) == -1 ){
    close(pfp[0]);
    close(pfp[1]);
    return {NULL, 0};
  }

  if ( pid > 0 ){	
    if (close( pfp[child_end] ) == -1 )
      return {NULL, 0};
    return {fdopen( pfp[parent_end] , mode), pid};
  }

  if ( close(pfp[parent_end]) == -1 )
    exit(1);

  if ( dup2(pfp[child_end], child_end) == -1 )
    exit(1);

  if ( close(pfp[child_end]) == -1 )
    exit(1);
  execl( "/bin/sh", "sh", "-c", command, NULL );
  exit(1);
}

int BloqqerCaller::process(std::vector<int> &f, int vars, int jobId, int litToTry, int maxCost) {
  pid_t pid = getpid();
  std::string fifoPath = std::to_string(pid) + "." + std::to_string(jobId);
  std::string command = "build/qbf_bloqqer "
    + fifoPath
    + " --maxexpvarcost=" + std::to_string(maxCost)
    + " --expvar=" + std::to_string(litToTry);

  LOGGER(_log, V3_VERB, "Calling qbf_bloqqer with command %s\n", command.c_str());

  auto [bloqqer, bloqqer_pid] = popen2(command.c_str(), "r");
  _pid = bloqqer_pid;

  if(bloqqer) {
    FIFO fifo{fifoPath, 0600, "w"};
    writeQDIMACS(f, fifo.fifo, vars);
    fifo.~FIFO();
    readQDIMACS(bloqqer, f);
    fclose(bloqqer);
  }
  int wstatus = 0;
  waitpid(_pid, &wstatus, 0);
  _pid = 0;

  return WEXITSTATUS(wstatus);
}

void BloqqerCaller::writeQDIMACS(const std::vector<int> &src, FILE* tgt, int vars) {
  // There's one more 0 because of the prefix delimited to the matrix
  // by a 0. This has to be removed when counting the zeroes in the
  // vector.
  int clauses = std::count(src.begin(), src.end(), 0) - 1;
  fprintf(tgt, "p cnf %d %d\n", vars, clauses);

  size_t i = 0;

  // Prefix
  int last = 0;
  for(; src[i] != 0; ++i) {
    if(src[i] < 0 && last < 0) {
      fprintf(tgt, "%d ", -src[i]);
    } else if(src[i] > 0 && last > 0) {
      fprintf(tgt, "%d ", src[i]);
    } else if(src[i] < 0 && last > 0) {
      fprintf(tgt, "0\na %d ", -src[i]);
    } else if(src[i] > 0 && last < 0) {
      fprintf(tgt, "0\ne %d ", src[i]);
    } else if(src[i] < 0 && last == 0) {
      fprintf(tgt, "a %d ", -src[i]);
    } else if(src[i] > 0 && last == 0) {
      fprintf(tgt, "e %d ", src[i]);
    }
    last = src[i];
  }
  fprintf(tgt, "0\n");

  // Matrix
  for(i = i + 1; i < src.size(); ++i) {
    if(src[i] == 0) {
      fprintf(tgt, "0\n");
    } else {
      fprintf(tgt, "%d ", src[i]);
    }
  }
}
bool BloqqerCaller::readQDIMACS(FILE* src, std::vector<int> &tgt, bool keepPrefix) {
  if(keepPrefix) {
    auto prefixEnd = std::find(tgt.begin(), tgt.end(), 0);
    ++prefixEnd;
    tgt.erase(prefixEnd, tgt.end());
  } else {
    tgt.clear();
  }

  int vars, clauses;
  int read_items = fscanf(src, "p cnf %d %d\n", &vars, &clauses);

  if(read_items != 2) return false;

  tgt.reserve(vars + clauses * 5);

  // Prefix

  int sign = 0;
  
  while(true) {
    char q = fgetc(src);
    if(q == 'a') {
      sign = -1;
    } else if(q == 'e') {
      sign = 1;
    } else {
      ungetc(q, src);
      break;
    }

    // Remove the space following the quantifier symbol
    char c = fgetc(src);
    if(c != ' ') return false;

    int v = 0;
    while(fscanf(src, "%d", &v) == 1 && v != 0) {
      if(!keepPrefix) {
        tgt.emplace_back(v * sign);
      }
    }
    c = fgetc(src);
    if(c != '\n') return false;
  }

  // Spacer between prefix and matrix.
  if(!keepPrefix)
    tgt.emplace_back(0);

  int v;
  while(fscanf(src, "%d", &v) == 1) {
    tgt.emplace_back(v);
  }

  return true;
}

void BloqqerCaller::kill() {
  pid_t p = _pid;
  ::kill(p, SIGINT);
}
