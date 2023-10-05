#include "bloqqer_caller.hpp"
#include "util/logger.hpp"
#include "util/sys/fileutils.hpp"

#include <cstdlib>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

BloqqerCaller::BloqqerCaller() {

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
  std::string fifoPath = "/tmp/mallob.bloqqer." + std::to_string(pid) + "." + std::to_string(jobId);
  std::string command = "build/qbf_bloqqer "
    + fifoPath
    + " --maxexpvarcost=" + std::to_string(maxCost)
    + " --expvar=" + std::to_string(litToTry);

  int res = mkfifo(fifoPath.c_str(), 0600);
  if (res != 0) {
    LOG(V0_CRIT, "[ERROR] Could not make FIFO, res=%i err=%s\n", res,
        strerror(errno));
    abort();
  }

  auto [bloqqer, bloqqer_pid] = popen2(command.c_str(), "r");
  _pid = bloqqer_pid;

  if(bloqqer) {
    FILE* fifo = fopen(fifoPath.c_str(), "w");
    if (fifo == NULL) {
      LOG(V0_CRIT, "[ERROR] Could not open FIFO, err=%s\n", strerror(errno));
      abort();
    }
    writeQDIMACS(f, fifo, vars);
    fclose(fifo);
    readQDIMACS(bloqqer, f, false);
    fclose(bloqqer);
  }
  int wstatus = 0;
  waitpid(_pid, &wstatus, 0);
  _pid = 0;

  FileUtils::rm(fifoPath);

  int exitcode = WEXITSTATUS(wstatus);
  return exitcode;
}

int BloqqerCaller::computeNumberOfVars(const std::vector<int> &f) const {
  int max = 0;
  for(int e : f) {
    int e_abs = abs(e);
    if(e_abs > max) {
      max = e_abs;
    }
  }
  return max;
}

#if __has_cpp_attribute(unlikely)
#define UNLIKELY [[unlikely]]
#else
#define UNLIKELY
#endif

#define CHECK(PRINT) if(PRINT < 0) UNLIKELY return

void BloqqerCaller::writeQDIMACS(const std::vector<int> &src, FILE* tgt, int vars) {
  // There's one more 0 because of the prefix delimited to the matrix
  // by a 0. This has to be removed when counting the zeroes in the
  // vector.
  int clauses = std::count(src.begin(), src.end(), 0) - 1;
  CHECK(fprintf(tgt, "p cnf %d %d\n", vars, clauses));

  size_t i = 0;

  // Prefix
  int last = 0;
  for(; src[i] != 0; ++i) {
    if(src[i] < 0 && last < 0) {
      CHECK(fprintf(tgt, "%d ", -src[i]));
    } else if(src[i] > 0 && last > 0) {
      CHECK(fprintf(tgt, "%d ", src[i]));
    } else if(src[i] < 0 && last > 0) {
      CHECK(fprintf(tgt, "0\na %d ", -src[i]));
    } else if(src[i] > 0 && last < 0) {
      CHECK(fprintf(tgt, "0\ne %d ", src[i]));
    } else if(src[i] < 0 && last == 0) {
      CHECK(fprintf(tgt, "a %d ", -src[i]));
    } else if(src[i] > 0 && last == 0) {
      CHECK(fprintf(tgt, "e %d ", src[i]));
    }
    last = src[i];
  }
  CHECK(fprintf(tgt, "0\n"));

  // Matrix
  for(i = i + 1; i < src.size(); ++i) {
    if(src[i] == 0) {
      CHECK(fprintf(tgt, "0\n"));
    } else {
      CHECK(fprintf(tgt, "%d ", src[i]));
    }
  }
}

#undef UNLIKELY
#undef CHECK

bool BloqqerCaller::readQDIMACS(FILE* src, std::vector<int> &tgt, bool keepPrefix) {
  if(keepPrefix) {
    auto prefixEnd = std::find(tgt.begin(), tgt.end(), 0);
    ++prefixEnd;
    tgt.erase(prefixEnd, tgt.end());
  } else {
    tgt.clear();
  }

  int read_items = fscanf(src, "p cnf %d %d\n", &_vars, &_clauses);

  if(read_items != 2) return false;

  if(_vars == 0)
    return true;

  tgt.reserve(_vars + _clauses * 5);

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
