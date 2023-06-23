#pragma once

#include <string>
#include <vector>

class BloqqerCaller {
public:
  BloqqerCaller();
  ~BloqqerCaller();

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

  int computeNumberOfVars(const std::vector<int> &f) const;

  void writeQDIMACS(const std::vector<int> &src, FILE* tgt, int vars);
  bool readQDIMACS(FILE* src, std::vector<int> &tgt, bool keepPrefix = false);

  int getVars() const { return _vars; }
  int getClauses() const { return _clauses; }

  void kill();
private:
  volatile pid_t _pid = 0;
  int _vars = 0;
  int _clauses = 0;
};
