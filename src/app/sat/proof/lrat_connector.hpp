
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "app/sat/data/clause.hpp"
#include "app/sat/parse/serialized_formula_parser.hpp"
#include "app/sat/proof/impcheck.hpp"
#include "app/sat/proof/lrat_op_tamperer.hpp"
#include "app/sat/proof/trusted_checker_process_adapter.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "trusted/trusted_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/terminator.hpp"

class LratConnector {

private:
    Logger& _logger;
    const int _base_seed;
    const int _local_id;
    SPSCBlockingRingbuffer<LratOp> _ringbuf;

    TrustedCheckerProcessAdapter _checker;
    BackgroundWorker _bg_acceptor;
    BackgroundWorker _bg_emitter;

    ProbingLearnedClauseCallback _cb_probe;
    LearnedClauseCallback _cb_learn;

    bool _launched {false};
    bool _unsat_validated {false};
    bool _sat_validated {false};
    volatile bool _sat_validation_requested {false};

    // buffering
    static constexpr int MAX_CLAUSE_LENGTH {512};
    int _clause_lits[MAX_CLAUSE_LENGTH];
    Mallob::Clause _clause {_clause_lits, 0, 0};

    std::unique_ptr<SerializedFormulaParser> _f_parser;

    float _tampering_chance_per_mille {0};

public:
    LratConnector(Logger& logger, int baseSeed, int localId, int nbVars, bool checkModel) :
        _logger(logger), _base_seed(baseSeed), _local_id(localId), _ringbuf(1<<14),
        _checker(logger, baseSeed, _local_id, nbVars, checkModel) {}

    inline auto& getChecker() {
        return _checker;
    }

    void setLearnedClauseCallback(const LearnedClauseCallback& cb) {_cb_learn = cb;}
    void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& cb) {_cb_probe = cb;}
    void setTamperingChancePerMille(float chance) {
        _tampering_chance_per_mille = chance;
    }

    void launch(SerializedFormulaParser& fParser) {
        _f_parser.reset(new SerializedFormulaParser(fParser));

        if (_launched) return;
        _launched = true;

        // summary of formula for debugging
        //std::string summary;
        //for (size_t i = 0; i < std::min(5UL, _f_size); i++) summary += std::to_string(_f_data[i]) + " ";
        //if (_f_size > 10) summary += " ... ";
        //for (size_t i = std::max(5UL, _f_size-5); i < _f_size; i++) summary += std::to_string(_f_data[i]) + " ";
        //LOG(V2_INFO, "PROOF> got formula with %lu lits: %s\n", _f_size, summary.c_str());

        _bg_emitter.run([&]() {runEmitter();});
    }

    inline void push(LratOp&& op) {
        if (op.isDerivation()) {
            // Clauses which will potentially be shared need to be sorted.
            // We use the glue value as an indicator for sharing (0 = no sharing)
            // and probe if the clause is currently eligible for sharing.
            // If the clause is not eligible, we set the glue to zero.
            // This skips the computation of a clause signature in the checker
            // and signals to the acceptor thread that the clause is NOT to be shared.
            int& glue = op.glue();
            bool share = glue > 0 && _cb_probe(op.getNbLits());
            if (share) op.sortLiterals();
            else glue = 0;

            if (MALLOB_UNLIKELY(_tampering_chance_per_mille > 0)) {
                if (1000*Random::rand() <= _tampering_chance_per_mille) {
                    // Tampering with this derivation!
                    op.glue() = 0;
                    LratOpTamperer(_logger).tamper(op);
                }
            }
        }
        if (MALLOB_UNLIKELY(op.isImport() && _tampering_chance_per_mille > 0)) {
            if (1000*Random::rand() <= _tampering_chance_per_mille) {
                // Tampering with this import!
                LratOpTamperer(_logger).tamper(op);
            }
        }
        _ringbuf.pushBlocking(op);
    }
    bool setSolution(const int* modelData, size_t modelSize) {
        // setModel() is threadsafe and only succeeds for one single call
        bool success = _checker.setModel(modelData, modelSize);
        if (success) {
            // Signal a concluding request to the emitter thread.
            _sat_validation_requested = true;
            _ringbuf.markExhausted(); // "wakes up" blocked emitter thread
        }
        return success;
    }
    bool waitForUnsatValidation() {
        while (!_unsat_validated) usleep(1000);
        return _unsat_validated;
    }
    bool waitForSatValidation() {
        while (!_sat_validated) usleep(1000);
        return _sat_validated;
    }

    void stop() {

        // Tell solver and emitter thread to stop inserting/processing statements
        _ringbuf.markExhausted();
        _ringbuf.markTerminated();

        // Terminate the emitter thread
        _bg_emitter.stop();

        // Manually submit a termination sentinel to the checker process.
        LratOp end(0);
        _checker.submit(end);

        // Wait for the sentinel to make the round through the process
        // and back to the acceptor
        _bg_acceptor.join(); // NOT stop()! We want it to finish on its own!
        // Terminate signal arrived at the checker process, threads joined

        // Wait until the checker process did in fact exit
        _checker.terminate();
    }
    ~LratConnector() {
        stop();
    }

private:

    void runEmitter() {
        Proc::nameThisThread("LRATEmitter");

        // Load formula
        if (_f_parser->isCompressed()) {
            int lit;
            std::vector<int> buf;
            while (_f_parser->getNextLiteral(lit)) {
                do {
                    buf.push_back(lit);
                } while (buf.size() < (1UL<<14) && _f_parser->getNextLiteral(lit));
                _checker.load(buf.data(), buf.size());
                buf.clear();
            }
        } else {
            size_t offset = 0;
            size_t fSize = _f_parser->getPayloadSize();
            const int* fData = _f_parser->getRawPayload();
            while (_bg_emitter.continueRunning() && offset < fSize) {
                size_t nbInts = std::min(1UL<<14, fSize-offset);
                _checker.load(fData+offset, nbInts);
                offset += nbInts;
            }
            assert(offset == fSize);
        }
        if (_bg_emitter.continueRunning()) {

            // End loading, check signature
            bool ok = _checker.endLoading();
            if (!ok) abort();
        }

        // *Always* start acceptor to ensure sound termination
        _bg_acceptor.run([&]() {runAcceptor();});

        // Lrat operation emission loop
        LratOp op;
        while (_bg_emitter.continueRunning()) {
            if (MALLOB_UNLIKELY(_sat_validation_requested)) {
                // SAT validation requested, model is already present in _checker
                op = LratOp {10};
                _sat_validation_requested = false;
                // SAT op is submitted below
            } else if (!_ringbuf.pollBlocking(op)) { // poll for an op
                // no success
                continue;
            }
            //LOG(V2_INFO, "PROOF> submit %s\n", op.toStr().c_str());
            _checker.submit(op);
        }
    }

    void runAcceptor() {
        Proc::nameThisThread("LRATAcceptor");

        LratOp op;
        signature sig;
        while (true) { // This thread can only be terminated with a "termination" LratOp.
            bool res;
            bool ok = _checker.accept(op, res, sig);
            if (!ok) break; // terminated
            if (!res) continue; // error in checker - nonetheless, wait for proper termination
            //LOG(V2_INFO, "PROOF> accept (%i) %s\n", (int)res, op.toStr().c_str());
            if (op.isDerivation()) {
                bool share = op.getGlue() > 0;
                if (share) {
                    prepareClause(op, sig);
                    _cb_learn(_clause, _local_id);
                }
            } else if (op.isUnsatValidation()) {
                _unsat_validated = true;
                LOGGER(_logger, V2_INFO, "Use impcheck_confirm -key-seed=%lu to confirm the fingerprint\n",
                    ImpCheck::getKeySeed(_base_seed));
            } else if (op.isSatValidation()) {
                _sat_validated = true;
            } else if (op.isTermination()) {
                break; // end
            }
        }
    }

    inline void prepareClause(LratOp& op, const u8* sig) {
        _clause.size = 2+4+op.getNbLits();
        assert(_clause.size <= MAX_CLAUSE_LENGTH);
        auto id = op.getId();
        memcpy(_clause.begin, &id, 8);
        memcpy(_clause.begin+2, sig, SIG_SIZE_BYTES);
        memcpy(_clause.begin+2+4, op.getLits(), op.getNbLits()*sizeof(int));
        _clause.lbd = op.getGlue();
        if (op.getNbLits() == 1) _clause.lbd = 1;
        else _clause.lbd = std::min(_clause.lbd, _clause.size);
    }
};
