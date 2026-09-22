
#pragma once

#include <vector>

#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/solvers/kissat.hpp"
#include "app/sat/solvers/lingeling.hpp"
#include "app/sat/solvers/solver_portfolio_config.hpp"
#include "app/satwithpre/sat_preprocess_actor.hpp"
#include "data/job_description.hpp"
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/thread_pool.hpp"
#include <future>
#include <sys/stat.h>
#include <unistd.h>

class KissatPreprocessor : public SatPreprocessActor {

private:
    std::unique_ptr<Kissat> _kissat;
    SolverPortfolioConfig& _spc;

public:
    KissatPreprocessor(const Parameters& params, const JobDescription& desc, const std::string& name,
            SolverPortfolioConfig& spc, std::vector<int>&& formula) :
        SatPreprocessActor(params, name, std::move(formula)), _spc(spc) {

        _proof_format = "drat";
    }

    void preprocessAsync() override {
        _fut_prepro = ProcessWideThreadPool::get().addTask([&]() {
            CoreAllocator::Allocation ca(1);

            SolverSetup setup;
            setup.logger = &Logger::getMainInstance();
            setup.numVars = nbInputVars();
            setup.numOriginalClauses = nbInputClauses();
            setup.solverType = 'k';
            setup.flavour = PortfolioSequence::PREPROCESS;
            setup.solverConfig = &_spc;
            if (_params.savePreprocessingProofs()) {
                setup.certifiedUnsat = true;
                setup.proofDir = _params.proofDirectory() + "/tmp/" + _name;
                FileUtils::mkdir(setup.proofDir);
            }
            _kissat.reset(new Kissat(setup));

            auto proofTracker = _kissat->getPreprocessProofTracker();
            for (int i = 0; i+2 < _input_cnf.size(); i++) {
                // Important: proof tracker import *before* Kissat import
                // (Otherwise Kissat may emit a proof line on a clause
                // the proof tracker doesn't know yet)
                proofTracker->appendOriginalLiteral(_input_cnf[i]);
                _kissat->addLiteral(_input_cnf[i]);
            }
            _kissat->diversify(0);
            _kissat->applySolverConfiguration(0);

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
                _kissat->cleanUp(); // force immediate clean up: finalizes proof, outputs CNF
                _result = SIMPLIFIED;
            } else {
                _result = NONE;
            }
        });
    }

    bool rename_proof(int i, std::string cnfSrc = "", std::string proofSrc = "") override {
        bool res = SatPreprocessActor::rename_proof(i,
            _params.proofDirectory() + "/tmp/" + _name + "/formula.cnf",
            _params.proofDirectory() + "/tmp/" + _name + "/proof.drat");
        return res;
    }

    void writeCnf(const std::vector<int>& cnf, std::string path = "") override {
        SatPreprocessActor::writeCnf(cnf,
            _params.proofDirectory() + "/tmp/" + _name + "/formula.cnf");
    }

    void reconstructSolution(std::vector<int>& sol) override {
        _kissat->reconstructSolutionFromPreprocessing(sol);
    }
};
