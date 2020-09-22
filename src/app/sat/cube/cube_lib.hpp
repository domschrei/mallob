#ifndef MSCHICK_CUBE_LIB_H
#define MSCHICK_CUBE_LIB_H

#include <vector>

#include "app/sat/hordesat/utilities/logging_interface.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"

class CubeLib {

private:
    std::vector<int> _formula;
    std::vector<std::vector<int>> _cubes;

    std::unique_ptr<LoggingInterface> _logger;
    std::unique_ptr<PortfolioSolverInterface> _solver;
    
public:
    CubeLib(std::vector<int> formula) : _formula(formula) {};

    void generateCubes();

    bool hasCubes() const;

    std::vector<int> getPreparedCubes();

    void digestCubes(std::vector<int> cubes);
};

#endif /* MSCHICK_CUBE_LIB_H */