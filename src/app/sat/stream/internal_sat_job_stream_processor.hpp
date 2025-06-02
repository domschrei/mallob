
#pragma once

#include <cstdlib>
#include <memory>
#include <stdlib.h>
#include <utility>
#include <vector>

#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/solvers/cadical.hpp"
#include "sat_job_stream_processor.hpp"
#include "util/logger.hpp"

class InternalSatJobStreamProcessor : public SatJobStreamProcessor {

private:
    std::unique_ptr<Cadical> _solver;
    int _current_rev {-1};
    volatile bool _pending {false};
    volatile bool _interrupted {false};

public:
    InternalSatJobStreamProcessor(bool incremental, Synchronizer& sync) :
        SatJobStreamProcessor(sync) {

        SolverSetup setup;
        setup.logger = &Logger::getMainInstance();
        setup.solverType = 'C';
        setup.isJobIncremental = incremental;
        setup.exportClauses = false;
        _solver.reset(new Cadical(setup));
        _solver->setLearnedClauseCallback([&](const Mallob::Clause&, int) {});
        _solver->getTerminator().setExternalTerminator([&]() {return _terminator(_current_rev);});
    }

    ~InternalSatJobStreamProcessor() override {}

    virtual void setName(const std::string& baseName) override {
        _name = baseName + ":seq";
    }

    virtual void process(SatTask& task) override {
        _current_rev = task.rev;
        _interrupted = false;
        _pending = true;
        for (int lit : task.lits) _solver->addLiteral(lit);
        _solver->unsetSolverInterrupt();
        int res = _solver->solve(task.assumptions.size(), task.assumptions.data());
        std::vector<int> solution;
        if (res == 10) solution = _solver->getSolution();
        if (res == 20) for (int lit : _solver->getFailedAssumptions()) solution.push_back(lit);
        bool winner = concludeRevision(task.rev, res, std::move(solution));
        if (winner) LOG(V2_INFO, "%s rev. %i won with res=%i\n", _name.c_str(), task.rev, res);
        _pending = false;
    }

    virtual void finalize() override {
        SatJobStreamProcessor::finalize();
        _solver->setSolverInterrupt();
        while (_pending) {usleep(1000);}
    }
};
