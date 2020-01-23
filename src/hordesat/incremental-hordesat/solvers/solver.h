
#ifndef HORDESAT_MALLOB_SOLVER_H
#define HORDESAT_MALLOB_SOLVER_H

#include "utilities/Threading.h"
#include "solvers/PortfolioSolverInterface.h"

class Solver {

private:
    int nodeRank;
    int numNodes;
    int internalSolverIdx;

    PortfolioSolverInterface* solverInterface;
    Thread* thread;

    std::shared_ptr<LoggingInterface> logger;

public:
    void diversify(int mode);
    

};


#endif