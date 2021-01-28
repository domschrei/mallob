#ifndef MSCHICK_DYNAMIC_CUBE_SETUP_H
#define MSCHICK_DYNAMIC_CUBE_SETUP_H

#include <vector>

#include "../hordesat/utilities/default_logging_interface.hpp"
#include "../../../util/params.hpp"
#include "../hordesat/solvers/portfolio_solver_interface.hpp"

struct DynamicCubeSetup {
    std::shared_ptr<std::vector<int>> formula;
    LoggingInterface &logger;
    const Parameters &params;
    SatResult &result;

    DynamicCubeSetup(std::shared_ptr<std::vector<int>> formula, LoggingInterface &logger, const Parameters &params, SatResult &result)
        : formula(formula), logger(logger), params(params), result(result) {}
        
};




#endif /* MSCHICK_DYNAMIC_CUBE_SETUP_H */