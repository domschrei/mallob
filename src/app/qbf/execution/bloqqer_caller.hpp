#pragma once

#include "util/logger.hpp"

#include <string>
#include <vector>

class BloqqerCaller {
public:
  BloqqerCaller(Logger &logger);
  ~BloqqerCaller();

  struct FIFO {
    FIFO(std::string path, mode_t mode, const char* mode_);
    ~FIFO();
    FILE* fifo;
  };

  /* Regular process when calling Bloqqer:

     First call: Check if PCNF can be expanded, i.e. costs are below a
     threshold. If above threshold, Bloqqer returns the pre-processed
     formula and gives return code 2. If below threshold, Bloqqer
     expands the variable and pre-processes the formula, giving return
     code 1.

     Bloqqer may also give return code 10 or 20, saying the formula is
     trivially SAT or UNSAT.
   */
  int process(std::vector<int> &f, int vars, int jobId, int litToTry = 0, int maxCost = 10000);

  void writeQDIMACS(const std::vector<int> &src, FILE* tgt, int vars);
  void readQDIMACS(FILE* src, std::vector<int> &tgt);
  
private:
  Logger &_log;
};
