
#pragma once

#include <vector>

#include "app/sat/execution/solver_setup.hpp"
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

class LingelingPreprocessor : public SatPreprocessActor {

public:
    LingelingPreprocessor(const Parameters& params, const JobDescription& desc, const std::string& name, std::vector<int>&& formula) :
        SatPreprocessActor(params, name, std::move(formula)) {}
    ~LingelingPreprocessor() {}

    void preprocessAsync() override {
        _fut_prepro = ProcessWideThreadPool::get().addTask([&]() {
            CoreAllocator::Allocation ca(1);

            SolverSetup setup;
            setup.logger = &Logger::getMainInstance();
            setup.numVars = nbInputVars();
            setup.numOriginalClauses = nbInputClauses();
            setup.solverType = 'l';
            setup.flavour = PortfolioSequence::PREPROCESS;
            std::unique_ptr<Lingeling> _lingeling(new Lingeling(setup));

            for (int i = 0; i+2 < _input_cnf.size(); i++) {
                _lingeling->addLiteral(_input_cnf[i]);
            }
            _lingeling->diversify(0);

            LOG(V2_INFO, "PREPRO running Lingeling\n");
            int res = _lingeling->solve(0, nullptr);
            LOG(V2_INFO, "PREPRO Lingeling done, result %i\n", res);
            if (res == 10) _result = SAT;
            if (res == 20) _result = UNSAT;
            _result = NONE;
        });
    }

    // Nothing to do
    void reconstructSolution(std::vector<int>& sol) override {}
};
