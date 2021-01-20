#ifndef MSCHICK_CUBE_SETUP_H
#define MSCHICK_CUBE_SETUP_H

#include <vector>

#include "cube_communicator.hpp"
#include "../hordesat/utilities/default_logging_interface.hpp"
#include "../../../util/params.hpp"
#include "../hordesat/solvers/portfolio_solver_interface.hpp"

struct CubeSetup {
    std::shared_ptr<std::vector<int>> formula;
    CubeCommunicator &cube_comm;
    LoggingInterface &logger;
    const Parameters &params;
    SatResult &result;

    CubeSetup(std::shared_ptr<std::vector<int>> formula, CubeCommunicator &cube_comm, LoggingInterface &logger, const Parameters &params, SatResult &result)
        : formula(formula), cube_comm(cube_comm), logger(logger), params(params), result(result) {}
        
};




#endif /* MSCHICK_CUBE_SETUP_H */