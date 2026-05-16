
#pragma once

#include <vector>

#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/solvers/kissat.hpp"
#include "app/sat/solvers/lingeling.hpp"
#include "app/satwithpre/sat_preprocess_actor.hpp"
#include "data/job_description.hpp"
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/thread_pool.hpp"
#include <future>
#include <sys/stat.h>
#include <unistd.h>

class KissatPreprocessor : public SatPreprocessActor {

private:
    std::unique_ptr<Kissat> _kissat;

public:
    KissatPreprocessor(const Parameters& params, const JobDescription& desc, const std::string& name, std::vector<int>&& formula) :
        SatPreprocessActor(params, name, std::move(formula)) {}
    ~KissatPreprocessor() {}

    void preprocessAsync() override {
        _fut_prepro = ProcessWideThreadPool::get().addTask([&]() {
            CoreAllocator::Allocation ca(1);

            SolverSetup setup;
            setup.logger = &Logger::getMainInstance();
            setup.numVars = nbInputVars();
            setup.numOriginalClauses = nbInputClauses();
            setup.solverType = 'p';
            setup.flavour = PortfolioSequence::PREPROCESS;
            _kissat.reset(new Kissat(setup));

            for (int i = 0; i+2 < _input_cnf.size(); i++) {
                _kissat->addLiteral(_input_cnf[i]);
            }
            _kissat->diversify(0);

            LOG(V2_INFO, "PREPRO running Kissat\n");
            int res = _kissat->solve(0, nullptr);
            LOG(V2_INFO, "PREPRO Kissat done, result %i\n", res);
            if (res == 10) {
                _model = _kissat->getSolution();
                _result = SAT;
            } else if (res == 20) {
                _result = UNSAT;
            } else if (_kissat->hasPreprocessedFormula()) {
                _output_cnf = std::move(_kissat->extractPreprocessedFormula());
                _result = SIMPLIFIED;
            } else {
                _result = NONE;
            }
        });
    }

    void reconstructSolution(std::vector<int>& sol) override {
        _kissat->reconstructSolutionFromPreprocessing(sol);
    }
};
