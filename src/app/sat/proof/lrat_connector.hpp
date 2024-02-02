
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "app/sat/data/clause.hpp"
#include "app/sat/proof/trusted/trusted_solving.hpp"
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
    const int _local_id;
    SPSCBlockingRingbuffer<LratOp> _ringbuf;

    TrustedCheckerProcessAdapter _checker;
    BackgroundWorker _bg_acceptor;
    BackgroundWorker _bg_emitter;

    ProbingLearnedClauseCallback _cb_probe;
    LearnedClauseCallback _cb_learn;

    bool _launched {false};
    bool _unsat_validated {false};

    // buffering
    static constexpr int MAX_CLAUSE_LENGTH {512};
    int _clause_lits[MAX_CLAUSE_LENGTH];
    Mallob::Clause _clause {_clause_lits, 0, 0};

    const int* _f_data;
    size_t _f_size;

public:
    LratConnector(Logger& logger, int localId, int nbVars) : _local_id(localId), _ringbuf(1<<16),
        _checker(logger, _local_id, nbVars) {}

    inline auto& getChecker() {
        return _checker;
    }

    void setLearnedClauseCallback(const LearnedClauseCallback& cb) {_cb_learn = cb;}
    void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& cb) {_cb_probe = cb;}

    void launch(const int* fData, size_t fSize) {
        if (_launched) return;
        _launched = true;

        _f_data = fData;
        _f_size = fSize;

        // summary of formula for debugging
        //std::string summary;
        //for (size_t i = 0; i < std::min(5UL, _f_size); i++) summary += std::to_string(_f_data[i]) + " ";
        //if (_f_size > 10) summary += " ... ";
        //for (size_t i = std::max(5UL, _f_size-5); i < _f_size; i++) summary += std::to_string(_f_data[i]) + " ";
        //LOG(V2_INFO, "PROOF> got formula with %lu lits: %s\n", _f_size, summary.c_str());

        _bg_emitter.run([&]() {runEmitter();});
    }

    inline void push(LratOp&& op) {
        _ringbuf.pushBlocking(op);
    }
    bool waitForValidation() {
        while (!_unsat_validated) usleep(1000);
        return _unsat_validated;
    }

    ~LratConnector() {
        // If no background thread was ever launched, we need to submit
        // a termination directive to the checker process manually here.
        if (!_launched) {
            LratOp end(0);
            _checker.submit(end);
        }
        _ringbuf.markExhausted();
    }

private:

    void runEmitter() {
        LratOp end(0);

        // Load formula
        size_t offset = 0;
        while (_bg_emitter.continueRunning() && offset < _f_size) {
            size_t nbInts = std::min(1UL<<16, _f_size-offset);
            _checker.load(_f_data+offset, nbInts);
            offset += nbInts;
        }
        if (!_bg_emitter.continueRunning()) {
            _checker.submit(end);
            return;
        }
        assert(offset == _f_size);

        // End loading, check signature
        bool ok = _checker.endLoading();
        if (!ok) abort();

        // Start acceptor
        _bg_acceptor.run([&]() {runAcceptor();});

        // Lrat operation emission loop
        LratOp op;
        while (_bg_emitter.continueRunning()) {
            // Wait for an op, then poll it
            bool ok = _ringbuf.pollBlocking(op);
            if (!ok) continue;
            //LOG(V2_INFO, "PROOF> submit %s\n", op.toStr().c_str());
            _checker.submit(op);
        }

        // Termination sentinel
        _checker.submit(end);
    }

    void runAcceptor() {
        LratOp op;
        signature sig;
        while (_bg_acceptor.continueRunning()) {
            bool res;
            bool ok = _checker.accept(op, res, sig);
            if (!ok) break;
            //LOG(V2_INFO, "PROOF> accept (%i) %s\n", (int)res, op.toStr().c_str());
            if (!res) abort();
            if (op.isDerivation()) {
                bool share = op.getGlue() > 0 && _cb_probe(op.getNbLits());
                if (share) {
                    prepareClause(op, sig);
                    _cb_learn(_clause, _local_id);
                }
            } else if (op.isUnsatValidation()) {
                _unsat_validated = true;
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
