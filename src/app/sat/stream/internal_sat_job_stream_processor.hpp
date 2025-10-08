
#pragma once

#include <cstdlib>
#include <memory>
#include <stdlib.h>
#include <utility>
#include <vector>

#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/parse/serialized_formula_parser.hpp"
#include "app/sat/proof/lrat_connector.hpp"
#include "app/sat/solvers/cadical.hpp"
#include "sat_job_stream_processor.hpp"
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"

class InternalSatJobStreamProcessor : public SatJobStreamProcessor {

private:
    std::unique_ptr<Cadical> _solver;
    int _current_rev {-1};
    int _internal_rev {-1};
    volatile bool _pending {false};

    LratConnector* _lrat ;

public:
    InternalSatJobStreamProcessor(SolverSetup setup, Synchronizer& sync) :
        SatJobStreamProcessor(sync) {

        setup.logger = &Logger::getMainInstance();
        setup.localId = 0;
        setup.globalId = 0;
        setup.maxNumSolvers = 1;
        setup.solverType = 'C';
        setup.exportClauses = false;
        if (setup.onTheFlyChecking) setup.certifiedUnsat = true;
        _solver.reset(new Cadical(setup));
        _lrat = setup.onTheFlyChecking ? _solver->getLratConnector() : nullptr;

        _solver->setLearnedClauseCallback([&](const Mallob::Clause&, int) {});
        _solver->getTerminator().setExternalTerminator([&]() {
            return _terminator(_current_rev);
        });
        if (_lrat) _lrat->init(".seq");
    }

    ~InternalSatJobStreamProcessor() override {
        finalize(); // interrupt solving, prepare cleanup
        while (_pending) {usleep(1000);} // blocks until solver returned
        if (_lrat) _lrat->stop(); // blocks until checker is cleaned up
    }

    virtual void setName(const std::string& baseName) override {
        _name = baseName + ":seq";
    }

    virtual void process(SatTask& task) override {
        CoreAllocator::Allocation ca(1);
        _current_rev = task.rev;
        _internal_rev++;
        _pending = true;
        LOG(V2_INFO, "%s rev. %i process payload of size %lu, %lu assumptions\n", _name.c_str(), task.rev, task.lits.size(), task.assumptions.size());
        if (task.type == SatJobStreamProcessor::SatTask::RAW) {
            SerializedFormulaParser parser(Logger::getMainInstance(), task.lits.data(), task.lits.size(), true);
            assert(task.assumptions.empty());
            if (_lrat) _lrat->initiateRevision(_internal_rev, parser);
            int lit;
            while (parser.getNextLiteral(lit)) _solver->addLiteral(lit);
            while (parser.getNextAssumption(lit)) task.assumptions.push_back(lit);
        } else {
            for (int lit : task.lits) _solver->addLiteral(lit);
        }
        _solver->unsetSolverInterrupt();
        int res = _solver->solve(task.assumptions.size(), task.assumptions.data());
        std::vector<int> solution;
        if (res == 10) {
            solution = _solver->getSolution();
            if (_lrat) {
                LOG(V3_VERB, "%s Validating SAT ...\n", _name.c_str());
                // omit first "0" in solution vector
                _lrat->push(LratOp(solution.data()+1, solution.size()-1), true, _internal_rev);
                // Whether or not this was successful (another thread might have been earlier),
                // wait until SAT was validated.
                _lrat->waitForConclusion(_internal_rev);
            }
        }
        if (res == 20) {
            auto failed = _solver->getFailedAssumptions();
            auto failedVec = std::vector<int>(failed.begin(), failed.end());
            if (_lrat) {
                LOG(V3_VERB, "%s Validating UNSAT ...\n", _name.c_str());
                auto id = _solver->getUnsatConclusionId();
                assert(id != 0);
                _lrat->push(LratOp(id, failedVec.data(), failedVec.size()), true, _internal_rev);
                _lrat->waitForConclusion(_internal_rev);
            }
            solution = std::move(failedVec);
        }
        bool winner = concludeRevision(task.rev, res, std::move(solution));
        if (winner) LOG(V2_INFO, "%s rev. %i won with res=%i\n", _name.c_str(), task.rev, res);
        _pending = false;
    }

    virtual void finalize() override {
        SatJobStreamProcessor::finalize();
        _solver->setSolverInterrupt();
        if (_lrat) _lrat->prepareStop(); // does not block
    }
};
