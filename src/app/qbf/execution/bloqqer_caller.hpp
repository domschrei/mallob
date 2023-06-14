#pragma once

#include <string>
#include <vector>

class BloqqerCaller {
public:
  using PCNF = std::vector<int>;
  
  BloqqerCaller();
  ~BloqqerCaller();

  struct FIFO {
    FIFO(std::string path, mode_t mode);
    ~FIFO();
    int fd;
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
  int process(PCNF &inOut, int jobId, int litToTry = 0, int maxCost = 10000);
private:
};
